#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include "anarcast.h"
#include "graphs.h"
#include "crypt.c"

// how many blocks should be transfer at a time?
#define CONCURRENCY   8

// a node in our linked list of servers
struct node {
    unsigned int addr;
    char hash[HASHLEN];
    struct node *prev, *next;
};

// the head of our linked list of servers
struct node *head;

// our inform server hostname
char *inform_server;

// connect to the inform server and populate our lovely AVL tree
void inform ();

// read the transaction type, call the right function, and close the connection
void * run_thread (void *arg);

// mandatory variadic function
void alert (const char *s, ...);

// check if bit (db * cb) is set in g->graph
int is_set (struct graph *g, int db, int cb);

// read data, send back key, insert blocks, etc
void insert (int c);

// insert blocks that aren't set true at mask[block]
void do_insert (const char *blocks, const char *mask, int blockcount, int blocksize, const char *hashes);

// read key from client, download blocks, reconstruct data, insert reconstructed parts, etc
void request (int c);

// download all the blocks i can get, set mask[block] = 1 for each begot block
void do_request (char *blocks, char *mask, int blockcount, int blocksize, const char *hashes);

// add a reference to our lovely AVL tree. it better not be a duplicate!
void addref (unsigned int addr);

// remove a reference to our lovely AVL tree. it better be there!
void rmref (unsigned int addr);

// return the address of the proper host for hash
unsigned int route (const char hash[HASHLEN], int off);

int
main (int argc, char **argv)
{
    int l, c;
    pthread_t t;
    
    if (argc != 2) {
	fprintf(stderr, "Usage: %s <inform server>\n", argv[0]);
        exit(2);
    }
    
    chdir_to_home();
    inform((inform_server = argv[1]));
    l = listening_socket(PROXY_SERVER_PORT, INADDR_LOOPBACK);
    
    // accept connections and spawn a thread for each
    for (;;)
	if ((c = accept(l, NULL, 0)) != -1) {
	    int *i = malloc(4);
	    *i = c;
	    if (pthread_create(&t, NULL, run_thread, i) != 0)
		die("pthread_create() failed");
	    if (pthread_detach(t) != 0)
		die("pthread_detach() failed");
	}
}

void *
run_thread (void *arg)
{
    int c = *(int*)arg;
    char d;

    // read transaction type, call handler
    if (read(c, &d, 1) == 1) {
        if (d == 'r') request(c);
        if (d == 'i') insert(c);
    }

    printf("\n");
    if (close(c) == -1)
	die("close() failed");
    free(arg);
    pthread_exit(NULL);
}

void
alert (const char *s, ...)
{
    va_list args;
    va_start(args, s);
    printf("\n");
    vprintf(s, args);
    fflush(stdout);
    va_end(args);
}

//=== graph =================================================================

int
is_set (struct graph *g, int db, int cb)
{
    int n = (db * g->cbc) + cb;
    return (g->graph[n / 8] << (n % 8)) & 128;
}

//=== insert ================================================================

void
insert (int c)
{
    char *hashes, *blocks;
    unsigned int i, j, datalength;
    unsigned int blocksize, len, hlen, dlen, clen;
    struct graph g;
    
    // read data length in bytes
    if (readall(c, &datalength, 4) != 4) {
	alert("Error reading data length from client.");
	return;
    }
    
    // find the graph for this datablock count
    blocksize = 64 * sqrt(datalength);
    if (datalength/blocksize > graphcount) {
	alert("I do not have a graph for %d data blocks.", datalength/blocksize);
	return;
    }
    g = graphs[datalength/blocksize-1];
    
    // allocate space for plaintext hash and data- and check-block hashes
    hlen = (1 + g.dbc + g.cbc) * HASHLEN;
    hashes = malloc(hlen);
    
    // pad to first multiple of our crypto blocksize
    while (g.dbc * blocksize < datalength + (datalength % 16))
	blocksize++;
    
    dlen = g.dbc * blocksize;
    clen = g.cbc * blocksize;
    len  = dlen + clen;
    
    // read data from client
    alert("Reading plaintext from client.");
    blocks = mbuf(len);
    memset(&blocks[i], 0, dlen - i);
    if (readall(c, blocks, datalength) != datalength) {
	alert("Error reading data from client.");
	if (munmap(blocks, len) == -1)
	    die("munmap() failed");
	free(hashes);
	return;
    }
    
    // hash data
    alert("Hashing data.");
    hashdata(blocks, datalength, hashes);
    
    // encrypt data
    alert("Encrypting data.");
    encryptdata(blocks, datalength + 16 - (datalength % 16), hashes);
    
    // generate check blocks
    alert("Generating %d check blocks for %d data blocks.", g.cbc, g.dbc);
    for (i = 0 ; i < g.cbc ; i++) {
	char b[1024];
	sprintf(b, "Check block %2d:", i+1);
	for (j = 0 ; j < g.dbc ; j++)
	    if (is_set(&g, j, i)) {
		xor(&blocks[dlen+(i*blocksize)], &blocks[j*blocksize], blocksize);
		sprintf(b, "%s %d", b, j+1);
	    }
	alert("%s.", b);
    }
    
    alert("Hashing blocks.");
    
    // generate data block hashes
    for (i = 0 ; i < g.dbc ; i++)
	hashdata(&blocks[i*blocksize], blocksize, &hashes[(i+1)*HASHLEN]);
    
    // generate check block hashes
    for (i = 0 ; i < g.cbc ; i++)
	hashdata(&blocks[dlen+(i*blocksize)], blocksize, &hashes[(g.dbc+1)*HASHLEN+(i*HASHLEN)]);
    
    // send the URI to the client
    i = hlen + 4;
    if (writeall(c, &i, 4) != 4 ||
	writeall(c, &datalength, 4) != 4 ||
	writeall(c, hashes, hlen) != hlen) {
	alert("Writing key to client failed.");
	if (munmap(blocks, len) == -1)
	    die("munmap() failed");
	free(hashes);
	return;
    }

    // actually insert the blocks
    alert("Inserting %d blocks of %d bytes each.", g.dbc + g.cbc, blocksize);
    do_insert(blocks, NULL, g.dbc + g.cbc, blocksize, &hashes[HASHLEN]);
    alert("Blocks inserted.");
    
    if (munmap(blocks, len) == -1)
	die("munmap() failed");
    free(hashes);
}

int
hookup (const char hash[HASHLEN], int off)
{
    struct sockaddr_in a;
    extern int errno;
    int c;

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(ANARCAST_SERVER_PORT);
    a.sin_addr.s_addr = route(hash, off);

    if ((c = socket(AF_INET, SOCK_STREAM, 0)) == -1)
	die("socket() failed");
    
    set_nonblock(c);

    // loop until connect works
    for (;;)
	if (connect(c, &a, sizeof(a)) == -1 && errno != EINPROGRESS) {
	    rmref(a.sin_addr.s_addr);
	    a.sin_addr.s_addr = route(hash, 0);
	} else break;
    
    return c;
}

void
do_insert (const char *blocks, const char *mask, int blockcount, int blocksize, const char *hashes)
{
    int m, next, active;
    fd_set w;
    
    struct {
	int num;
	int off;
    } xfers[FD_SETSIZE];
    
    FD_ZERO(&w);
    next = active = 0;
    m = 1;
    
    for (;;) {
	int i;
	fd_set x = w;

	if (active) {
	    i = select(m, NULL, &x, NULL, NULL);
	    if (i == -1) die("select() failed");
	    if (!i) continue;
	}

	// make new connections
	while (active < CONCURRENCY && next < blockcount) {
	    int c;
	    // skip this part, its mask is true
	    if (mask && mask[next]) {
		next++;
		continue;
	    }
	    // connect to server, watch fd
	    c = hookup(&hashes[next*HASHLEN], 0);
	    FD_SET(c, &w);
	    if (c >= m) m = c + 1;
	    xfers[c].num = next;
	    xfers[c].off = -5; // 'i' + 4-byte datalength
	    active++;
	    next++;
	}

	// send data to eligible servers
	for (i = 0 ; i < m ; i++)
	    if (FD_ISSET(i, &x)) {
		int n = xfers[i].off;
		if (n == -5)
		    // command
		    n = writeall(i, "i", 1);
		else if (n < 0)
		    // data length
		    n = writeall(i, &(&blocksize)[4+n], -n);
		else
		    // data
		    n = writeall(i, &blocks[xfers[i].num*blocksize+n], blocksize-n);

		// io error
		if (n <= 0) {
		    int c;
		    if (close(i) == -1)
			die("close() failed");
		    FD_CLR(i, &w);
		    // connect to server, watch new fd
		    c = hookup(&hashes[xfers[i].num*HASHLEN], 0);
		    FD_SET(c, &w);
		    xfers[c].num = xfers[i].num;
		    xfers[c].off = -5; // 'i' + 4-byte datalength
		}
		
		// are we done?
		xfers[i].off += n;
		if (xfers[i].off == blocksize) {
		    if (close(i) == -1)
			die("close() failed");
		    FD_CLR(i, &w);
		    if (i == m) m--;
		    active--;
		}
	    }

	// nothing left to do?
	if (!active && next == blockcount)
	    break;
    }
}

//=== request ===============================================================

void
request (int c)
{
    int i, a, m, n;
    unsigned int datalength, blockcount, blocksize;
    char *blocks, *mask, *mask2, hash[HASHLEN], *hashes;
    struct graph g;
    
    // read key length (a key is datalength + hashes)
    if (readall(c, &i, 4) != 4) {
	alert("Error reading key length from client.");
	return;
    }
    
    // is our key a series of > 2 hashes?
    i -= 4; // subtract datalength bytes
    if (i % HASHLEN || i == HASHLEN) {
	alert("Bad key length: %s.", i);
	return;
    }

    // read datalength and hashes from client
    hashes = malloc(i);
    if (readall(c, &datalength, 4) != 4 || readall(c, hashes, i) != i) {
	alert("Error reading key from client.");
	free(hashes);
	return;
    }
    
    // find the graph for this datablock count
    blocksize = 64 * sqrt(datalength);
    if (datalength/blocksize > graphcount) {
	alert("I do not have a graph for %d data blocks.", datalength/blocksize);
	return;
    }
    g = graphs[datalength/blocksize-1];
    
    // pad to first multiple of our crypto blocksize
    while (g.dbc * blocksize < datalength + (datalength % 16))
	blocksize++;
    
    blockcount = g.dbc + g.cbc;
    mask = malloc(blockcount);
    mask2 = malloc(blockcount);
    blocks = mbuf(blockcount * blocksize);
    
    // slurp up all the data we can
    alert("Downloading %d blocks of %d bytes each.", blockcount, blocksize);
    memset(mask, 0, blockcount); // all parts are missing before we download them
    do_request(blocks, mask, blockcount, blocksize, &hashes[HASHLEN]);
    
    // how many missing blocks?
    for (i = n = 0 ; i < blockcount ; i++)
	if (!mask[i]) n++;
    
    alert("Download of %d/%d (%d%%) blocks completed.", blockcount-n, blockcount,
	  (int) ((double) (blockcount-n) / (double) blockcount * 100));
    
    if (!(m = n)) {
	alert("No missing parts to reconstruct.");
	goto verify; // woo! we got all of `em!
    }
    
    // back up original mask. we'll use it to insert the reconstructed parts later
    memcpy(mask2, mask, blockcount);
    
    // try to reconstruct blocks until we win or lose
    do {
	int j, k;
	char b[1024];
	
	a = 0; // no blocks built yet
	
	// try to reconstruct missing data blocks. to reconstruct a data block,
	// we need to have a check block of which it's a member, and the other
	// data blocks that were xored into that check block.
	for (i = 0 ; i < g.dbc ; i++) {
	    if (mask[i]) continue; // already built
	    // find any check blocks this data block is in
	    for (j = 0 ; j < g.cbc ; j++) {
		if (!mask[g.dbc+j] || !is_set(&g, i, j))
		    continue; // not built yet or not a relevant check block
		// do we have all the other data blocks in this check block?
		for (k = 0 ; k < g.dbc ; k++)
		    if (is_set(&g, k, j) && !mask[k] && k != i)
			goto next; // we don't have all the data blocks yet.
		// yay! we have both the check block and the other data blocks.
		// xor them all together, and we have our missing data block!
		sprintf(b, "Computed data block %d from check block %d and data blocks:", i+1, j+1);
		xor(&blocks[i*blocksize], &blocks[(g.dbc+j)*blocksize], blocksize);
		for (k = 0 ; k < g.dbc ; k++)
		    if (is_set(&g, k, j) && k != i) {
			sprintf(b, "%s %d", b, k+1);
			xor(&blocks[i*blocksize], &blocks[k*blocksize], blocksize);
		    }
		alert("%s.", b);
		mask[i] = a = 1; // we got it, baby!
		n--;
		break;
	     next:;
	    }
	}

	// try to reconstruct missing check blocks. to reconstruct a check block,
	// we need to have all its data blocks.
	for (i = 0 ; i < g.cbc ; i++) {
	    if (mask[g.dbc+i]) continue; // already built
	    // do we have all the data blocks?
	    for (j = 0 ; j < g.dbc ; j++)
		if (is_set(&g, j, i) && !mask[j])
		    goto next2;
	    // woohoo! we have all the data blocks. we'll xor them to make a check block!
	    sprintf(b, "Computed check block %d from data blocks:", i+1);
	    for (j = 0 ; j < g.dbc ; j++)
		if (is_set(&g, j, i)) {
		    sprintf(b, "%s %d", b, j+1);
		    xor(&blocks[(g.dbc+i)*blocksize], &blocks[j*blocksize], blocksize);
		}
	    alert("%s.", b);
	    mask[g.dbc+i] = a = 1; // we got it, baby!!
	    n--;
	 next2:;
	}

	if (!n || a) continue; // the following is expensive, so avoid it if possible

	// we may still be able to do it. there's a possibility that we can
	// reconstruct a data block from check blocks alone. first, we find two
	// check blocks that include the same lowest-numbered data block. next,
	// we kabootie flimwuggle gribble wonk
	
    } while (a && n);
    
    if (n) { // damn, we just couldn't do it
	char b[1024];
	sprintf(b, "Data was not recoverable. %d unrecovered blocks:", n);
	for (i = 0 ; i < blockcount ; i++)
	    if (!mask[i]) sprintf(b, "%s %d", b, i+1);
	alert("%s.", b);
	goto out;
    }
    
verify:
    // decrypt data
    alert("Decrypting data.");
    decryptdata(blocks, datalength + 16 - (datalength % 16), hashes);

    // verify data
    hashdata(blocks, datalength, hash);
    if (memcmp(hash, hashes, HASHLEN)) {
	alert("Data integrity did not verify.");
	goto out;
    }
    alert("Data integrity verified.");
    
    // write data to client
    if (writeall(c, &datalength, 4) != 4 || writeall(c, blocks, datalength) != datalength) {
	alert("Error writing data to client.");
	goto out;
    }
    alert("%d bytes written to client.", datalength);
    
    if (!m) goto out; // no blocks to reinsert!
    
    // verify integrity of reconstructed check blocks
    for (i = 0 ; i < g.cbc ; i++)
	if (!mask2[g.dbc+i]) {
	    hashdata(&blocks[(g.dbc+i)*blocksize], blocksize, hash);
	    if (memcmp(hash, &hashes[(1+g.dbc+i)*HASHLEN], HASHLEN)) {
		alert("Check block %d does not verify.", i+1);
		goto out;
	    }
	}
    
    // insert reconstructed blocks
    alert("Inserting %d reconstructed blocks.", m);
    do_insert(blocks, mask2, blockcount, blocksize, &hashes[HASHLEN]);
    alert("Reconstructed blocks inserted.");

out:
    if (munmap(blocks, blockcount * blocksize) == -1)
	die("munmap() failed");
    free(hashes);
    free(mask);
    free(mask2);
}

void
do_request (char *blocks, char *mask, int blockcount, int blocksize, const char *hashes)
{
    int m, next, active;
    fd_set r, w;
    
    struct {
	int num;
	int try;
	int off;
	int dlen;
    } xfers[FD_SETSIZE];
    
    FD_ZERO(&r);
    FD_ZERO(&w);
    next = active = 0;
    m = 1;
    
    for (;;) {
	int i;
	fd_set s = r, x = w;

	if (active) {
	    i = select(m, &s, &x, NULL, NULL);
	    if (i == -1) die("select() failed");
	    if (!i) continue;
	}

	// make new connections
	while (active < CONCURRENCY && next < blockcount) {
	    int c;
	    // skip this part, its mask is true
	    if (mask && mask[next]) {
		next++;
		continue;
	    }
	    // connect to server, watch fd
	    c = hookup(&hashes[next*HASHLEN], 0);
	    FD_SET(c, &w);
	    if (c >= m) m = c + 1;
	    xfers[c].num = next;
	    xfers[c].try = 0;
	    xfers[c].off = -1; // 'r'
	    active++;
	    next++;
	}

	// send request to eligible servers
	for (i = 0 ; i < m ; i++)
	    if (FD_ISSET(i, &x)) {
		int n = xfers[i].off;
		if (n == -1)
		    // command
		    n = writeall(i, "r", 1);
		else
		    // hash
		    n = writeall(i, &hashes[xfers[i].num*HASHLEN+n], HASHLEN-n);

		// io error
		if (n <= 0) {
		    if (close(i) == -1)
			die("close() failed");
		    FD_CLR(i, &w);
		    // connect to server, watch new fd
		    if (++xfers[i].try < 3) {
		        int c = hookup(&hashes[xfers[i].num*HASHLEN], xfers[c].try);
		        FD_SET(c, &w);
		        xfers[c].num = xfers[i].num;
			xfers[c].try = xfers[i].try;
		        xfers[c].off = -1; // 'r'
		    } else {
			if (i == m) m--;
		        active--;
		    }
		}
		
		// are we done sending our request?
		if ((xfers[i].off += n) == HASHLEN) {
		    FD_CLR(i, &w); // no more sending data...
		    FD_SET(i, &r); // reading is good fer yer brane!
		    xfers[i].off = -4; // datalength
		}
	    }

	// read our precious data
	for (i = 0 ; i < m ; i++)
	    if (FD_ISSET(i, &s)) {
		int n = xfers[i].off;
		if (n < 0)
		    // data length
		    n = readall(i, &(&xfers[i].dlen)[4+n], -n);
		else
		    // data
		    n = readall(i, &blocks[xfers[i].num*blocksize+n], blocksize-n);

		if (n <= 0) {
		    if (close(i) == -1)
		        die("close() failed");
		    FD_CLR(i, &r);
		    // try another server
		    if (++xfers[i].try < 3) {
		        int c = hookup(&hashes[xfers[i].num*HASHLEN], xfers[i].try);
		        FD_SET(c, &w);
		        xfers[c].num = xfers[i].num;
		        xfers[c].try = xfers[i].try;
		        xfers[c].off = -(1+HASHLEN); // 'r' + hash
		    } else {
		        if (i == m) m--;
		        active--;
		    }
		}

		// is the data length incorrect?
		xfers[i].off += n;
		if (!xfers[i].off && xfers[i].dlen != blocksize) {
		    alert("Data length read for block %d is incorrect. (%d != %d)", xfers[i].num+1, xfers[i].dlen, blocksize);
		    if (close(i) == -1)
			die("close() failed");
		    FD_CLR(i, &r);
		    if (i == m) m--;
		    active--;
		}

		// are we done reading the data?
		if (xfers[i].off == blocksize) {
		    char hash[HASHLEN];
		    hashdata(&blocks[xfers[i].num*blocksize], blocksize, hash);
		    if (memcmp(&hashes[xfers[i].num*HASHLEN], hash, HASHLEN))
			alert("Integrity of block %d does not verify.", xfers[i].num+1);
		    else
			mask[xfers[i].num] = 1; // success
		    
		    if (close(i) == -1)
			die("close() failed");
		    FD_CLR(i, &r);
		    if (i == m) m--;
		    active--;
		}
	    }

	// nothing left to do?
	if (!active && next == blockcount)
	    break;
    }
}

//=== inform ================================================================

void
inform ()
{
    int c, n;
    unsigned int count;
    struct sockaddr_in a;
    struct hostent *h;
    extern int h_errno;
    
    if (!(h = gethostbyname(inform_server))) {
	alert("%s: %s.", inform_server, hstrerror(h_errno));
	exit(1);
    }
    
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(INFORM_SERVER_PORT);
    a.sin_addr.s_addr = ((struct in_addr *)h->h_addr)->s_addr;
    
    if ((c = socket(AF_INET, SOCK_STREAM, 0)) == -1)
	die("socket() failed");
    
    if (connect(c, &a, sizeof(a)) == -1)
	die("connect() failed");
    
    // how many friends do we have?
    if (readall(c, &count, 4) != 4)
	die("inform server hung up unexpectedly");
    
    if (!count) {
	puts("No servers, exiting.");
	exit(0);
    }
    
    head = NULL;
    
    // read and insert our friends
    for (n = 0 ; n < count ; n++) {
	unsigned int i;
	if (readall(c, &i, 4) != 4)
	    die("inform server hung up unexpectedly");
	addref(i);
    }

    if (close(c) == -1)
	die("close() failed");

    alert("%d Anarcast servers loaded.\n", count);
}

//=== routing ===============================================================

void
refop (char op, char *hash, unsigned int addr)
{
    char hex[HASHLEN*2+1];
    struct in_addr x;
    
    // print our pretty message
    x.s_addr = addr;
    bytestohex(hex, hash, HASHLEN);
    alert("%c %15s %s", op, inet_ntoa(x), hex);
}

void
addref (unsigned int addr)
{
    struct node *n;
    
    n = malloc(sizeof(struct node));
    n->addr = addr;
    hashdata(&addr, 4, n->hash);
    
    if (!head) {
	n->next = n->prev = NULL;
	head = n;
    } else {
	struct node *p;
	for (p = head ; ; p = p->next) {
    	    if (memcmp(n->hash, p->hash, HASHLEN) < 0) {
		if (p->prev) { // middle
		    n->next = p;
		    n->prev = p->prev;
		    p->prev->next = n;
		    p->prev = n;
		} else { // first
		    n->next = head;
		    n->prev = NULL;
		    head->prev = n;
		    head = n;
		}
		break;
	    } else if (!p->next) { // last
		n->next = NULL;
		n->prev = p;
		p->next = n;
		break;
	    }
	}
    }

    refop('+', n->hash, n->addr);
}

void
rmref (unsigned int addr)
{
    char hash[HASHLEN];
    struct node *p;
    
    hashdata(&addr, 4, hash);
    
    for (p = head; p ; p = p->next)
	if (!memcmp(p->hash, hash, HASHLEN)) {
	    if (p->prev) {
		p->prev->next = p->next;
		if (p->next)
		    p->next->prev = p->prev;
	    } else { // first
		head = p->next;
		if (p->next)
		    p->next->prev = NULL;
	    }
	    free(p);
	    p = NULL;
	    refop('-', hash, addr);
	    return;
	}
    
    die("address not found in linked list");
}

unsigned int
route (const char hash[HASHLEN], int off)
{
    struct node *p;
    
    assert(off < 3);
    
    if (!head)
	die("empty address list");
    
    for (p = head ; ; p = p->next)
	if (!p->next || memcmp(hash, p->hash, HASHLEN) < 0)
	    break;
    
    if (off) {
	if (p->next && !p->prev)
	    p = p->next;
	else if (!p->next && p->prev)
	    p = p->prev;
	else if (p->next && p->prev) {
	    if (memcmp(hash, p->prev->hash, HASHLEN) > memcmp(hash, p->next->hash, HASHLEN))
	        p = off == 1 ? p->next : p->prev;
	    else
	        p = off == 1 ? p->prev : p->next;
	}
    }
    
    refop('*', p->hash, p->addr);
    
    return p->addr;
}

