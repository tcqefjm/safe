#include <crypto/hash.h>
#include <crypto/skcipher.h>

static void generate_key(unsigned char * key)
{
	struct shash_desc sdesc;
	uid_t uid = current_euid().val;
	short i;

	sdesc.tfm = crypto_alloc_shash("crc32-pclmul", 0, 0);
	crypto_shash_digest(& sdesc, (char *)& uid, sizeof(uid_t), key + 28);
	for (i = 28; i > 0; i -= 4)
	{
		crypto_shash_digest(& sdesc, key + i, 32 - i, key + i - 4);
	}
	crypto_free_shash(sdesc.tfm);
}

static void generate_iv(char * iv, unsigned long inode, loff_t offset)
{
	struct shash_desc sdesc;
	short i;

	sdesc.tfm = crypto_alloc_shash("crc32-pclmul", 0, 0);
	crypto_shash_digest(& sdesc, (char *)& inode, sizeof(unsigned long), iv + 4);
	crypto_shash_digest(& sdesc, iv + 4, 4, iv);
	crypto_free_shash(sdesc.tfm);
	for (i = 0; i < sizeof(loff_t); ++i)
	{
		iv[15 - i] = ((char *)& offset)[i];
	}
}

/*
** For the purpose of read/write random access, we choose AES CTR mode to transform plain/cipher.
** The key is generated from owner uid, the iv is generated from file inode and read/write position.
*/
static void transform(char * buf, unsigned long inode, loff_t offset, size_t count)
{
	struct crypto_skcipher * skcipher = NULL;
	struct skcipher_request * req = NULL;
	unsigned char key[32] = { 0 };
	char ivdata[16] = { 0 };
	char prefix[15] = { 0 };
	short pre_len = offset & 0xf;
	struct scatterlist sg;

	skcipher = crypto_alloc_skcipher("ctr-aes-aesni", 0, 0);
	req = skcipher_request_alloc(skcipher, GFP_KERNEL);
	generate_key(key);
	crypto_skcipher_setkey(skcipher, key, 32);
	generate_iv(ivdata, inode, offset >> 4);
	buf -= pre_len;
	sg_init_one(& sg, buf, count + pre_len);
	skcipher_request_set_crypt(req, & sg, & sg, count + pre_len, ivdata);
	memcpy(prefix, buf, pre_len);
	crypto_skcipher_encrypt(req);
	memcpy(buf, prefix, pre_len);
	buf += pre_len;

	crypto_free_skcipher(skcipher);
	skcipher_request_free(req);
}
