#include "system.h"
#include "sccs.h"
#define	LTC_SOURCE
#include "tomcrypt.h"
#include "tomcrypt/randseed.h"
#include "tomcrypt/oldrsa.h"
#include "cmd.h"

private	int	make_keypair(int bits, char *secret, char *public);
private	int	signdata(rsa_key *secret);
private	int	validatedata(rsa_key *public, char *sign);
private	int	encrypt_stream(rsa_key *public, FILE *fin, FILE *fout);
private	int	decrypt_stream(rsa_key	*secret, FILE *fin, FILE *fout);
private void	loadkey(char *file, rsa_key *key);

private	int		wprng = -1;
private	prng_state	*prng;
private	int	use_sha1_hash = 0;
private	int	hex_output = 0;
private	int	rsakey_old = 0;

private const u8	pubkey5[151] = {
~~~~~~~~~~~~~~~~~~~~~0h
~~~~~~~~~~~~~~~~~~~~~~~C
~~~~~~~~~~~~~~~~~~~~~~R
~~~~~~~~~~~~~~~~~~~~~~x
~~~~~~~~~~~~~~~~~~~~~~0g
~~~~~~~~~~~~~~~~~~~~~~A
~~~~~~~~~~~~~~~~~~~~~~0Y
~~~~~~~~~~~~~~~~~~~~~~0j
~~~~~~~~~~~~~~~~~~~~~r
~~~~~~~~~~~~~~~~~~~~~~~+
~~~~~~~0\
};
private const u8	pubkey6[151] = {
~~~~~~~~~~~~~~~~~~~~~;
~~~~~~~~~~~~~~~~~~~~~~:
~~~~~~~~~~~~~~~~~~~~~t
~~~~~~~~~~~~~~~~~~~~~~t
~~~~~~~~~~~~~~~~~~~~~~~(
~~~~~~~~~~~~~~~~~~~~~~g
~~~~~~~~~~~~~~~~~~~~~~~%
~~~~~~~~~~~~~~~~~~~~~~
~~~~~~~~~~~~~~~~~~~~~~?
~~~~~~~~~~~~~~~~~~~~~~D
~~~~~~~~~~w
};
private const u8	upgrade_secretkey[828] = {
~~~~~~~~~~~~~~~~~~~~~~~~~~~4
~~~~~~~~~~~~~~~~~~~~~~~~~~~I
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~7
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~0^
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~G
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~J
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~N
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~S
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~0O
~~~~~~~~~~~~~~~~~~~~~~~~~~~~0T
~~~~~~~~~~~~~~~~~~~~~~~~~~~~0V
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~]
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~:
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~i
~~~~~~~~~~~~~~~~~~~~~~~~~~~~0Z
~~~~~~~~~~~~~~~~~~~~~~~~~~~~+
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~N
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~0T
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~0[
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~<
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~/
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~0\
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~0c
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~0V
~~~~~~~~~~~~~~~~~~~~~~~~~~~~0U
~~~~~~~~~~~~~~~~~~~~~~~~~~~0f
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~m
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~H
~~~~~~~~~~~~~~~~~~~~~~~~~~~~S
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~m
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~{
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~-
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~J
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~s
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~@
~~~~~~~~~~~~~~~~~~~~~~~~~~~~)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~0Q
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~t
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~;
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~X
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~0]
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~0R
~~~~~~~~~~~~~~~~~~~~~~~~~~~~-
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~d
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~0\
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~0a
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~Q
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~]
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~>
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~0S
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~i
~~~~~~~~~~~~~~~~~~~~~~~~~~~~0\
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~o
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~7
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~p
~~~~~~~~~~~~~~~~~~~~~~~~~~~0l
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~w
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~2
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~z
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~s
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~!
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~W
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~0P
~~~~~~~~~~~~~~~~~~~~~~~~~~~~0Q
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~0X
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~0c
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~0e
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~E
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~5
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~g
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~D
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~0Z
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~0]
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~_
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~;
~~~~~~~~~~~~~~~~~~~~~~~~~~~h
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~m
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~0n
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~0
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~#
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~h
~~~~~~~~~~~~~~~~~~~~~~~?
};

/*
 * -i bits secret-key public-key
 *    # generate new random keypair and writes the two keys to files
 * -e key < plain > cipher
 *    # encrypt a datastream on stdin and write the data to stdout
 *    # normally the public key is used for this
 * -d key < cipher > plain
 *    # decrypt a datastream on stdin and write the data to stdout
 *    # normally the private key is used for this
 * -s key < data > sign
 *    # read data from stdin and write signature to stdout
 *    # normally private key is used for this
 * -v key sign < data
 *    # read signature from file and data from stdin
 *    # exit status indicates if the data matches signature
 *    # normally public key is used for this
 * -h <data> [<key>]
 *    # hash data with an optional key
 *
 * -E key < plain > cipher
 *    # simple aes symetric encryption of data
 *    # key must be 16 bytes long
 * -D key < plain > cipher
 *    # simple aes symetric decryption of data
 *    # key must be 16 bytes long
 * -S
 *    # use sha1 instead of md5 for -h
 * -X
 *    # print hash results in hex instead of base64 for -h
 * -O
 *    # load RSA key in old format
 */

private void
usage(void)
{
	system("bk help -s crypto");
	exit(1);
}

int
crypto_main(int ac, char **av)
{
	int	c;
	int	mode = 0;
	int	args, optargs;
	int	ret = 1;
	char	*hash;
	rsa_key	key;

	while ((c = getopt(ac, av, "dDeEhiOsSvX")) != -1) {
		switch (c) {
		    case 'h': case 'i': case 's': case 'v':
		    case 'e': case 'd': case 'E': case 'D':
			if (mode) usage();
			mode = c;
			break;
		    case 'S': use_sha1_hash = 1; break;
		    case 'X': hex_output = 1; break;
		    case 'O': rsakey_old = 1; break;
		    default:
			usage();
		}
	}
	optargs = 0;
	switch (mode) {
	    case 'i': args = 3; break;
	    case 'v': args = 2; break;
	    default: args = 1; break;
	    case 'h': args = 1; optargs = 1; break;
	}
	if (ac - optind < args || ac - optind > args + optargs) {
		fprintf(stderr, "ERROR: wrong number of args!\n");
		usage();
	}

	setmode(0, _O_BINARY);
	register_cipher(&rijndael_desc);
	register_hash(&md5_desc);
	wprng = rand_getPrng(&prng);

	switch (mode) {
	    case 'i':
		ret = make_keypair(atoi(av[optind]),
		    av[optind+1], av[optind+2]);
		break;
	    case 'e': case 'd': case 's': case 'v':
		loadkey(av[optind], &key);
		switch (mode) {
		    case 'e': ret = encrypt_stream(&key, stdin, stdout); break;
		    case 'd': ret = decrypt_stream(&key, stdin, stdout); break;
		    case 's': ret = signdata(&key); break;
		    case 'v': ret = validatedata(&key, av[optind+1]); break;
		}
		rsa_free(&key);
		break;
	    case 'E':
		if (strlen(av[optind]) == 16) {
			ret = crypto_symEncrypt(av[optind], stdin, stdout);
		} else {
			fprintf(stderr,
			    "ERROR: key must be exactly 16 bytes\n");
			ret = 1;
		}
		break;
	    case 'D':
		if (strlen(av[optind]) == 16) {
			ret = crypto_symDecrypt(av[optind], stdin, stdout);
		} else {
			fprintf(stderr,
			    "ERROR: key must be exactly 16 bytes\n");
			ret = 1;
		}
		break;
	    case 'h':
		if (av[optind+1]) {
			hash = secure_hashstr(av[optind], strlen(av[optind]),
			    av[optind+1]);
		} else {
			hash = hashstr(av[optind], strlen(av[optind]));
		}
		puts(hash);
		free(hash);
		ret = 0;
		break;
	    default:
		usage();
	}
	return (ret);
}

private	int
make_keypair(int bits, char *secret, char *public)
{
	rsa_key	key;
	unsigned long	size;
	FILE	*f;
	char	out[4096];
	int	err;

	if (err = rsa_make_key(prng, wprng, bits/8, 655337, &key)) {
		fprintf(stderr, "crypto: %s\n", error_to_string(err));
		return (1);
	}
	size = sizeof(out);
	if (err = rsa_export(out, &size, PK_PRIVATE, &key)) {
		fprintf(stderr, "crypto export private key: %s\n",
		    error_to_string(err));
		return (1);
	}
	f = fopen(secret, "w");
	unless (f) {
		fprintf(stderr, "crypto: can open %s for writing\n", secret);
		return(2);
	}
	fwrite(out, 1, size, f);
	fclose(f);
	size = sizeof(out);
	if (err = rsa_export(out, &size, PK_PUBLIC, &key)) {
		fprintf(stderr, "crypto exporting public key: %s\n",
		    error_to_string(err));
		return (1);
	}
	f = fopen(public, "w");
	unless (f) {
		fprintf(stderr, "crypto: can open %s for writing\n", public);
		return(2);
	}
	fwrite(out, 1, size, f);
	fclose(f);
	rsa_free(&key);
	return (0);
}

private void
loadkey(char *file, rsa_key *key)
{
	u8	*data;
	int	err, fsize;

	data = loadfile(file, &fsize);
	unless (data) {
		fprintf(stderr, "crypto: cannot load key from %s\n", file);
		exit(1);
	}
	if (rsakey_old) {
		err = oldrsa_import(data, key);
	} else {
		err = rsa_import(data, fsize, key);
	}
	if (err) {
		fprintf(stderr, "crypto loadkey: %s\n", error_to_string(err));
		exit(1);
	}
	free(data);
}

private	int
signdata(rsa_key *key)
{
	int	hash = find_hash("md5");
	unsigned long	hashlen, x;
	int	err;
	u8	hbuf[32], out[4096];

	/* are the parameters valid? */
	assert (hash_is_valid(hash) == CRYPT_OK);

	/* hash it */
	hashlen = sizeof(hbuf);
	if (err = hash_filehandle(hash, stdin, hbuf, &hashlen)) {
err:		fprintf(stderr, "crypto: %s\n", error_to_string(err));
		return (1);
	}

	x = sizeof(out);
	if (err = oldrsa_sign_hash(hbuf, hashlen, out, &x,
		hash_descriptor[hash].ID, key)) goto err;
	fwrite(out, 1, x, stdout);

	return (0);
}

private	int
validatedata(rsa_key *key, char *signfile)
{
	int		hash = register_hash(&md5_desc);
	u8		*sig;
	int		siglen;
	unsigned long	hashlen;
	int		stat, err;
	u8		hbuf[32];

	sig = loadfile(signfile, &siglen);
	unless (sig && (siglen > 16)) {
		fprintf(stderr, "crypto: unable to load signature\n");
		return (1);
	}
	hashlen = sizeof(hbuf);
	if (err = hash_filehandle(hash, stdin, hbuf, &hashlen)) {
error:		fprintf(stderr, "crypto: %s\n", error_to_string(err));
		return (1);
	}
	if (err = oldrsa_verify_hash(sig, &siglen, hbuf, hashlen,
		&stat, hash_descriptor[hash].ID, key)) goto error;
	return (stat ? 0 : 1);
}

int
check_licensesig(char *key, char *sign, int version)
{
	int		hash = register_hash(&md5_desc);
	unsigned long	hashlen, signbinlen;
	rsa_key		rsakey;
	u8		*pubkey;
	int		pubkeylen;
	int		err, i;
	int		stat;
	u8		hbuf[32];
	char		signbin[256];

	if (version <= 5) {
		pubkey = (u8 *)pubkey5;
		pubkeylen = sizeof(pubkey5);
	} else {
		pubkey = (u8 *)pubkey6;
		pubkeylen = sizeof(pubkey6);
	}
	if (err = oldrsa_import(pubkey, &rsakey)) {
err:		fprintf(stderr, "check_licensesig: %s\n",
		    error_to_string(err));
		exit(1);
	}
	signbinlen = sizeof(signbin);
	if (err = base64_decode(sign, strlen(sign), signbin, &signbinlen)) {
		goto err;
	}
	hashlen = sizeof(hbuf);
	if (err = hash_memory(hash, key, strlen(key), hbuf, &hashlen)) goto err;
	i = signbinlen;
	if (oldrsa_verify_hash(signbin, &i, hbuf, hashlen,
		&stat, hash_descriptor[hash].ID, &rsakey)) return (-1);
	rsa_free(&rsakey);
	return (stat ? 0 : 1);
}

int
base64_main(int ac, char **av)
{
	int	c, err;
	int	unpack = 0;
	long	len, outlen;
	char	buf[4096], out[4096];

	while ((c = getopt(ac, av, "d")) != -1) {
		switch (c) {
		    case 'd': unpack = 1; break;
		    default:
			system("bk help -s base64");
			exit(1);
		}
	}

	if (unpack) {
		setmode(fileno(stdout), _O_BINARY);
		while (fgets(buf, sizeof(buf), stdin)) {
			len = strlen(buf);
			outlen = sizeof(out);
			if (err = base64_decode(buf, len, out, &outlen)) {
err:				fprintf(stderr, "base64: %s\n",
				    error_to_string(err));
				return (1);
			}
			fwrite(out, 1, outlen, stdout);
		}
	} else {
		setmode(fileno(stdin), _O_BINARY);
		while (len = fread(buf, 1, 48, stdin)) {
			outlen = sizeof(out);
			if (err = base64_encode(buf, len, out, &outlen)) {
				goto err;
			}
			fwrite(out, 1, outlen, stdout);
			putchar('\n');
		}
	}
	return (0);
}

/* this seems to have been removed from libtomcrypt,
 * it seems to me it should really be a tomcrypt API
 */
private int
hmac_filehandle(int hash, FILE *in,
    const unsigned char *key, unsigned long keylen,
    unsigned char *out, unsigned long *outlen)
{
	hmac_state	hmac;
	unsigned char	buf[512];
	size_t		x;
	int		err;

	if (err = hash_is_valid(hash)) {
		return (err);
	}

	if (err = hmac_init(&hmac, hash, key, keylen)) {
		return (err);
	}

	while ((x = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (err = hmac_process(&hmac, buf, (unsigned long)x)) {
			return (err);
		}
	}

	if (err = hmac_done(&hmac, out, outlen)) {
		return (err);
	}
	zeromem(buf, sizeof(buf));
	return (CRYPT_OK);
}

char *
secure_hashstr(char *str, int len, char *key)
{
	int	hash = register_hash(use_sha1_hash ? &sha1_desc : &md5_desc);
	unsigned long md5len, b64len;
	char	*p;
	int	n;
	char	md5[32];
	char	b64[64];

	md5len = sizeof(md5);
	if (key && (len == 1) && streq(str, "-")) {
		if (hmac_filehandle(hash, stdin,
		    key, strlen(key), md5, &md5len)) {
			return (0);
		}
	} else if (key) {
		if (hmac_memory(hash, key, strlen(key),
			str, len, md5, &md5len)) return (0);
	} else if ((len == 1) && streq(str, "-")) {
		if (hash_filehandle(hash, stdin, md5, &md5len)) return (0);
	} else {
		if (hash_memory(hash, str, len, md5, &md5len)) return (0);
	}
	b64len = sizeof(b64);
	if (hex_output) {
		assert(sizeof(b64) > md5len*2+1);
		for (n = 0; n < md5len; n++) {
			sprintf(b64 + 2*n,
			    "%1x%x", (md5[n] >> 4) & 0xf, md5[n] & 0xf);
		}
	} else {
		if (base64_encode(md5, md5len, b64, &b64len)) return (0);
		for (p = b64; *p; p++) {
			if (*p == '/') *p = '-';	/* dash */
			if (*p == '+') *p = '_';	/* underscore */
			if (*p == '=') {
				*p = 0;
				break;
			}
		}
	}
	return (strdup(b64));
}

char *
hashstr(char *str, int len)
{
	return (secure_hashstr(str, len, 0));
}

char *
hashstream(FILE *f)
{
	int	hash = register_hash(&md5_desc);
	unsigned long md5len, b64len;
	char	*p;
	char	md5[32];
	char	b64[32];

	md5len = sizeof(md5);
	if (hash_filehandle(hash, f, md5, &md5len)) return (0);
	b64len = sizeof(b64);
	if (base64_encode(md5, md5len, b64, &b64len)) return (0);
	for (p = b64; *p; p++) {
		if (*p == '/') *p = '-';	/* dash */
		if (*p == '+') *p = '_';	/* underscore */
		if (*p == '=') {
			*p = 0;
			break;
		}
	}
	return (strdup(b64));
}

char *
signed_loadFile(char *filename)
{
	int	len;
	char	*p;
	char	*hash;
	char	*data = loadfile(filename, &len);

	unless (data && len > 0) return (0);
	p = data + len - 1;
	*p = 0;
	while ((p > data) && (*p != '\n')) --p;
	*p++ = 0;
	hash = secure_hashstr(data, (p - data - 1),
	    makestring(KEY_SIGNEDFILE));
	unless (streq(hash, p)) {
		free(data);
		data = 0;
	}
	free(hash);
	return (data);
}

int
signed_saveFile(char *filename, char *data)
{
	FILE	*f;
	char	*tmpf;
	char	*hash;

	tmpf = aprintf("%s.%u", filename, getpid());
	unless (f = fopen(tmpf, "w")) {
		return (-1);
	}
	hash = secure_hashstr(data, strlen(data), makestring(KEY_SIGNEDFILE));
	fprintf(f, "%s\n%s\n", data, hash);
	fclose(f);
	free(hash);
	rename(tmpf, filename);
	unlink(tmpf);
	free(tmpf);
	return (0);
}

private int
encrypt_stream(rsa_key *key, FILE *fin, FILE *fout)
{
	int	cipher = register_cipher(&rijndael_desc);
	long	inlen, outlen, blklen;
	int	i, err;
	symmetric_CTR	ctr;
	u8	sym_IV[MAXBLOCKSIZE];
	u8	skey[32];
	u8	in[4096], out[4096];

	/* generate random session key */
	i = 32;
	cipher_descriptor[cipher].keysize(&i);
	inlen = i;

	blklen = cipher_descriptor[cipher].block_length;
	rand_getBytes(skey, inlen);
	rand_getBytes(sym_IV, blklen);

	outlen = sizeof(out);
	if (err = oldrsa_encrypt_key(skey, inlen, out, &outlen,
	    prng, wprng, key)) {
err:		fprintf(stderr, "crypto encrypt: %s\n", error_to_string(err));
		return (1);
	}
	fwrite(out, 1, outlen, fout);
	fwrite(sym_IV, 1, blklen, fout);

	/* bulk encrypt */
	if (err = ctr_start(cipher, sym_IV, skey, inlen, 0,
	    CTR_COUNTER_LITTLE_ENDIAN, &ctr)) {
		goto err;
	}

	while ((i = fread(in, 1, sizeof(in), fin)) > 0) {
		ctr_encrypt(in, out, i, &ctr);
		fwrite(out, 1, i, fout);
	}
	return (0);
}

private int
decrypt_stream(rsa_key *key, FILE *fin, FILE *fout)
{
	int	cipher = register_cipher(&rijndael_desc);
	unsigned long	inlen, outlen, blklen;
	int	i, err;
	symmetric_CTR	ctr;
	u8	sym_IV[MAXBLOCKSIZE];
	u8	skey[32];
	u8	in[4096], out[4096];

	i = inlen = fread(in, 1, sizeof(in), fin);
	outlen = sizeof(out);
	if (err = oldrsa_decrypt_key(in, &inlen, out, &outlen, key)) {
err:		fprintf(stderr, "crypto decrypt: %s\n", error_to_string(err));
		return (1);
	}
	memcpy(skey, out, outlen);

	blklen = cipher_descriptor[cipher].block_length;
	assert(inlen + blklen < i);
	memcpy(sym_IV, in + inlen, blklen);

	/* bulk encrypt */
	if (err = ctr_start(cipher, sym_IV, skey, outlen, 0,
		    CTR_COUNTER_LITTLE_ENDIAN, &ctr)) {
		goto err;
	}
	i -= inlen + blklen;
	ctr_decrypt(in + inlen + blklen, out, i, &ctr);
	fwrite(out, 1, i, fout);

	while ((i = fread(in, 1, sizeof(in), fin)) > 0) {
		ctr_decrypt(in, out, i, &ctr);
		fwrite(out, 1, i, fout);
	}
	return (0);
}

int
upgrade_decrypt(FILE *fin, FILE *fout)
{
	rsa_key	rsakey;
	int	err;

	if (err = oldrsa_import((u8 *)upgrade_secretkey, &rsakey)) {
		fprintf(stderr, "crypto rsa_import: %s\n",
		    error_to_string(err));
		exit(1);
	}
	decrypt_stream(&rsakey, fin, fout);
	return (0);
}

/* key contains 16 bytes of data */
int
crypto_symEncrypt(char *key, FILE *fin, FILE *fout)
{
	int	cipher = register_cipher(&rijndael_desc);
	long	blklen;
	int	i;
	symmetric_CFB	cfb;
	u8	sym_IV[MAXBLOCKSIZE];
	u8	buf[4096];

	blklen = cipher_descriptor[cipher].block_length;
	assert(blklen == 16);  // aes
	memset(sym_IV, 0, blklen);

	cfb_start(cipher, sym_IV, key, 16, 0, &cfb);

	while ((i = fread(buf, 1, sizeof(buf), fin)) > 0) {
		cfb_encrypt(buf, buf, i, &cfb);
		fwrite(buf, 1, i, fout);
	}
	return (0);
}

int
crypto_symDecrypt(char *key, FILE *fin, FILE *fout)
{
	int	cipher = register_cipher(&rijndael_desc);
	long	blklen;
	int	i;
	symmetric_CFB	cfb;
	u8	sym_IV[MAXBLOCKSIZE];
	u8	buf[4096];

	blklen = cipher_descriptor[cipher].block_length;
	assert(blklen == 16);  // aes
	memset(sym_IV, 0, blklen);

	cfb_start(cipher, sym_IV, key, 16, 0, &cfb);

	while ((i = fread(buf, 1, sizeof(buf), fin)) > 0) {
		cfb_decrypt(buf, buf, i, &cfb);
		fwrite(buf, 1, i, fout);
	}
	return (0);
}


/*
 * only setup the special key in the environment for restricted commands
 */
void
bk_preSpawnHook(int flags, char *av[])
{
	rand_setSeed((flags & _P_DETACH) ? 0 : 1);
}


private const u8	leasekey[] = {
~~~~~~~~~~~~~~~~~~~~~~~~~~~~^
~~~~~~~~~~~~~~~~~~~~~~~~~~~~V
~~~~~~~~~~~~~~~~~~~~~~~~~~~M
~~~~v
};
private const u8	bk_auth_hmackey[] = {
~~~~~~~~~~~~~~~~~~~~~~~~~~~{
~~~~~~~~~~~~~~~~~~~~~~~~~~~0T
~~~~~~~~~~~~~~~~~~~~~~~~~~~~`
~~~~x
};
private const u8	lconfigkey[] = {
~~~~~~~~~~~~~~~~~~~~~~~~~~~~0l
~~~~~~~~~~~~~~~~~~~~~~~~~~~~A
~~~~~]
};
private const u8	upgradekey[] = {
~~~~~~~~~~~~~~~~~~~~~~~~~~~~0l
~~~~~~~~~~~~~~~~~~~~~~~~~~~~A
~~~~~]
};
private const u8	signedfilekey[] = {
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~G
~~~~~~~~~~~~~~~~~~~~~~~~~~~~E
~~~~~]
};
private const u8	seedkey[] = {
~~~~~~~~~~~~~~~~~~~~~~~~~~~0X
~~~~~~~~~~~~~~~~~~~~~~~~~~~~`
~~~~~~~~~~~~~~~~~~~~~~~~~~~Q
~~~~w
};
private const u8	eulakey[] = {
~~~~~~~~~~~~~~~~~~~~~~~~~~~0T
~~~~~~~~~~~~~~~y
};

struct {
	const	u8	*key;
	int		size;
} savedkeys[] = {
	{ leasekey, sizeof(leasekey) },			/* 0 */
	{ bk_auth_hmackey, sizeof(bk_auth_hmackey) },	/* 1 */
	{ lconfigkey, sizeof(lconfigkey) },		/* 2 */
	{ upgradekey, sizeof(upgradekey) },		/* 3 */
	{ signedfilekey, sizeof(signedfilekey) },	/* 4 */
	{ seedkey, sizeof(seedkey) },			/* 5 */
	{ eulakey, sizeof(eulakey) },			/* 6 */
	{ 0, 0 }
};


/*
 * keep strings from showing up in strings by encoding them in char
 * arrays, then rebuild the strings with this.  Out is one more
 * that in, as it is null terminated.
 *
 * The inverse, array generating routine is in a standalone
 * program ./hidestring (source in hidestring.c).
 */
char *
makestring(int keynum)
{
	const	char	*in;
	int		i, size;
	char		seed = 'Q';
	static	char	*out;

	unless (out) {
		size = 0;
		for (i = 0; savedkeys[i].key; i++) {
			if (size < savedkeys[i].size) size = savedkeys[i].size;
		}
		out = malloc(size + 1);
	}
	in = savedkeys[keynum].key;
	size = savedkeys[keynum].size;
	for (i = 0; i < size; i++) {
		out[i] = (seed ^= in[i]);
	}
	out[i] = 0;
	return (out);
}

/*
 * A little helper funtion to find hash conflicts for binpool tests
 * Ex: These 3 files are conflicts: 155 236 317
 */
int
findhashdup_main(int ac, char **av)
{
	hash	*h;
	int	cnt = 0, max;
	u32	i, len, a32;
	char	buf[64];

	if (!av[1] || av[2]) return (1);
	max = atoi(av[1]);
	h = hash_new(HASH_MEMHASH);

	for (i = 0; ; i++) {
		len = sprintf(buf, "%d\n", i);
		a32 = adler32(0, buf, len);

		unless (hash_insert(h, &a32, 4, buf, len)) {
			printf("dup %.*s && %.*s == %08x\n",
			    len-1, buf, h->vlen-1, h->vptr,
			    *(u32 *)h->kptr);
			if (++cnt >= max) return (0);
		}
	}
	return (0);
}
