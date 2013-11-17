#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/coff.h>
#include <assert.h>
#include <stdint.h>

/* http://www.delorie.com/djgpp/doc/coff/ */

typedef struct {
  unsigned short f_magic;         /* magic number             */
  unsigned short f_nscns;         /* number of sections       */
  unsigned int   f_timdat;        /* time & date stamp        */
  unsigned int   f_symptr;        /* file pointer to symtab   */
  unsigned int   f_nsyms;         /* number of symtab entries */
  unsigned short f_opthdr;        /* sizeof(optional hdr)     */
  unsigned short f_flags;         /* flags                    */
} FILHDR;

typedef struct {
  unsigned short magic;          /* type of file                         */
  unsigned short vstamp;         /* version stamp                        */
  unsigned int   tsize;          /* text size in bytes, padded to FW bdry*/
  unsigned int   dsize;          /* initialized data    "  "             */
  unsigned int   bsize;          /* uninitialized data  "  "             */
  unsigned int   entry;          /* entry pt.                            */
  unsigned int   text_start;     /* base of text used for this file      */
  unsigned int   data_start;     /* base of data used for this file      */
} AOUTHDR;

typedef struct {
  char           s_name[8];  /* section name                     */
  unsigned int   s_paddr;    /* physical address, aliased s_nlib */
  unsigned int   s_vaddr;    /* virtual address                  */
  unsigned int   s_size;     /* section size                     */
  unsigned int   s_scnptr;   /* file ptr to raw data for section */
  unsigned int   s_relptr;   /* file ptr to relocation           */
  unsigned int   s_lnnoptr;  /* file ptr to line numbers         */
  unsigned short s_nreloc;   /* number of relocation entries     */
  unsigned short s_nlnno;    /* number of line number entries    */
  unsigned int   s_flags;    /* flags                            */
} SCNHDR;

typedef struct {
  unsigned int  r_vaddr;   /* address of relocation      */
  unsigned int  r_symndx;  /* symbol we're adjusting for */
  unsigned short r_type;    /* type of relocation         */
} __attribute__((packed)) RELOC;

static void my_assert_(int line, const char *name, long v, long expect, int is_eq)
{
	int ok;
	if (is_eq)
		ok = (v == expect);
	else
		ok = (v != expect);

	if (!ok)
	{
		printf("%d: '%s' is %lx, need %s%lx\n", line, name,
			v, is_eq ? "" : "!", expect);
		exit(1);
	}
}
#define my_assert(v, exp) \
	my_assert_(__LINE__, #v, (long)(v), (long)(exp), 1)
#define my_assert_not(v, exp) \
	my_assert_(__LINE__, #v, (long)(v), (long)(exp), 0)

void parse_headers(FILE *f, unsigned int *base_out,
	long *sect_ofs, uint8_t **sect_data, long *sect_sz,
	RELOC **relocs, long *reloc_cnt)
{
	unsigned int base = 0;
	long opthdr_pos;
	long reloc_size;
	FILHDR hdr;
	AOUTHDR opthdr;
	SCNHDR scnhdr;
	int s, val;
	int ret;
	
	ret = fread(&hdr, 1, sizeof(hdr), f);
	my_assert(ret, sizeof(hdr));

	if (hdr.f_magic == 0x5a4d) // MZ
	{
		ret = fseek(f, 0x3c, SEEK_SET);
		my_assert(ret, 0);
		ret = fread(&val, 1, sizeof(val), f);
		my_assert(ret, sizeof(val));

		ret = fseek(f, val, SEEK_SET);
		my_assert(ret, 0);
		ret = fread(&val, 1, sizeof(val), f);
		my_assert(ret, sizeof(val));
		my_assert(val, 0x4550); // PE

		// should be COFF now
		ret = fread(&hdr, 1, sizeof(hdr), f);
		my_assert(ret, sizeof(hdr));
	}

	my_assert(hdr.f_magic, COFF_I386MAGIC);

	if (hdr.f_opthdr != 0)
	{
		opthdr_pos = ftell(f);

		if (hdr.f_opthdr < sizeof(opthdr))
			my_assert(1, 0);

		ret = fread(&opthdr, 1, sizeof(opthdr), f);
		my_assert(ret, sizeof(opthdr));
		my_assert(opthdr.magic, COFF_ZMAGIC);

		printf("text_start: %x\n", opthdr.text_start);

		if (hdr.f_opthdr > sizeof(opthdr)) {
			ret = fread(&base, 1, sizeof(base), f);
			my_assert(ret, sizeof(base));
			printf("base: %x\n", base);
		}
		ret = fseek(f, opthdr_pos + hdr.f_opthdr, SEEK_SET);
		my_assert(ret, 0);
	}

	for (s = 0; s < hdr.f_nscns; s++) {
		ret = fread(&scnhdr, 1, sizeof(scnhdr), f);
		my_assert(ret, sizeof(scnhdr));

		if (scnhdr.s_size != 0)
			break;
	}

	printf("s_name:   '%s'\n", scnhdr.s_name);
	printf("s_vaddr:  %x\n", scnhdr.s_vaddr);
	printf("s_size:   %x\n", scnhdr.s_size);
	printf("s_scnptr: %x\n", scnhdr.s_scnptr);
	printf("s_nreloc: %x\n", scnhdr.s_nreloc);
	printf("--\n");

	ret = fseek(f, scnhdr.s_scnptr, SEEK_SET);
	my_assert(ret, 0);

	*sect_data = malloc(scnhdr.s_size);
	my_assert_not(*sect_data, NULL);
	ret = fread(*sect_data, 1, scnhdr.s_size, f);
	my_assert(ret, scnhdr.s_size);

	*sect_ofs = scnhdr.s_scnptr;
	*sect_sz = scnhdr.s_size;

	ret = fseek(f, scnhdr.s_relptr, SEEK_SET);
	my_assert(ret, 0);

	reloc_size = scnhdr.s_nreloc * sizeof((*relocs)[0]);
	*relocs = malloc(reloc_size + 1);
	my_assert_not(*relocs, NULL);
	ret = fread(*relocs, 1, reloc_size, f);
	my_assert(ret, reloc_size);

	*reloc_cnt = scnhdr.s_nreloc;

	if (base != 0)
		*base_out = base + scnhdr.s_vaddr;
}

static int handle_pad(uint8_t *d_obj, uint8_t *d_exe, int maxlen)
{
	static const uint8_t p7[7] = { 0x8d, 0xa4, 0x24, 0x00, 0x00, 0x00, 0x00 };
	static const uint8_t p6[6] = { 0x8d, 0x9b, 0x00, 0x00, 0x00, 0x00 };
	static const uint8_t p5[5] = { 0x05, 0x00, 0x00, 0x00, 0x00 };
	static const uint8_t p4[4] = { 0x8d, 0x64, 0x24, 0x00 };
	static const uint8_t p3[3] = { 0x8d, 0x49, 0x00 };
	static const uint8_t p2[2] = { 0x8b, 0xff };
	static const uint8_t p1[1] = { 0x90 };
	int len;
	int i;

	for (i = 0; i < maxlen; i++)
		if (d_exe[i] != 0xcc)
			break;

	for (len = i; len > 0; )
	{
		i = len;
		if (i > 7)
			i = 7;

		switch (i) {
		#define CASE(x) \
		case sizeof(p ## x): \
			if (memcmp(d_obj, p ## x, sizeof(p ## x))) \
				return 0; \
			memset(d_obj, 0xcc, sizeof(p ## x)); \
			break;
		CASE(7)
		CASE(6)
		CASE(5)
		CASE(4)
		CASE(3)
		CASE(2)
		CASE(1)
		default:
			printf("%s: unhandled len: %d\n", __func__, len);
			return 0;
		#undef CASE
		}

		len -= i;
		d_obj += i;
	}

	return 1;
}

struct equiv_opcode {
	signed char len;
	signed char ofs;
	short cmp_rm;
	uint8_t v_masm[8];
	uint8_t v_masm_mask[8];
	uint8_t v_msvc[8];
	uint8_t v_msvc_mask[8];
} equiv_ops[] = {
	// cmp    $0x11,%ax
	{ 4, -1, 0,
	 { 0x66,0x83,0xf8,0x03 }, { 0xff,0xff,0xff,0x00 },
	 { 0x66,0x3d,0x03,0x00 }, { 0xff,0xff,0x00,0xff }, },
	// lea    -0x1(%ebx,%eax,1),%esi // op mod/rm sib offs
	// mov, test, imm grp 1
	{ 3, -2, 1,
	 { 0x8d,0x74,0x03 }, { 0xf0,0x07,0xc0 },
	 { 0x8d,0x74,0x18 }, { 0xf0,0x07,0xc0 }, },
	// movzbl 0x58f24a(%eax,%ecx,1),%eax
	{ 4, -3, 1,
	 { 0x0f,0xb6,0x84,0x08 }, { 0xff,0xff,0x07,0xc0 },
	 { 0x0f,0xb6,0x84,0x01 }, { 0xff,0xff,0x07,0xc0 }, },
	// inc/dec
	{ 3, -2, 1,
	 { 0xfe,0x4c,0x03 }, { 0xfe,0xff,0xc0 },
	 { 0xfe,0x4c,0x18 }, { 0xfe,0xff,0xc0 }, },
	// cmp
	{ 3, -2, 1,
	 { 0x38,0x0c,0x0c }, { 0xff,0xff,0xc0 },
	 { 0x38,0x0c,0x30 }, { 0xff,0xff,0xc0 }, },
	// test   %dl,%bl
	{ 2, -1, 1,
	 { 0x84,0xd3 }, { 0xfe,0xc0 },
	 { 0x84,0xda }, { 0xfe,0xc0 }, },
	// cmp	  r,r/m vs rm/r
	{ 2, 0, 1,
	 { 0x3a,0xca }, { 0xff,0xc0 },
	 { 0x38,0xd1 }, { 0xff,0xc0 }, },
	// rep + 66 prefix
	{ 2, 0, 0,
	 { 0xf3,0x66 }, { 0xfe,0xff },
	 { 0x66,0xf3 }, { 0xff,0xfe }, },
	// fadd   st, st(0) vs st(0), st
	{ 2, 0, 0,
	 { 0xd8,0xc0 }, { 0xff,0xf7 },
	 { 0xdc,0xc0 }, { 0xff,0xf7 }, },

	// broad filters (may take too much..)
	// testb  $0x4,0x1d(%esi,%eax,1)
	// movb, push, ..
	{ 3, -2, 1,
	 { 0xf6,0x44,0x06 }, { 0x00,0x07,0xc0 },
	 { 0xf6,0x44,0x30 }, { 0x00,0x07,0xc0 }, },
};

static int cmp_mask(uint8_t *d, uint8_t *expect, uint8_t *mask, int len)
{
	int i;

	for (i = 0; i < len; i++)
		if ((d[i] & mask[i]) != (expect[i] & mask[i]))
			return 1;

	return 0;
}

static int check_equiv(uint8_t *d_obj, uint8_t *d_exe, int maxlen)
{
	uint8_t vo, ve, vo2, ve2;
	int i, jo, je;
	int len, ofs;

	for (i = 0; i < sizeof(equiv_ops) / sizeof(equiv_ops[0]); i++)
	{
		struct equiv_opcode *op = &equiv_ops[i];

		len = op->len;
		if (maxlen < len)
			continue;

		ofs = op->ofs;
		if (cmp_mask(d_obj + ofs, op->v_masm,
			     op->v_masm_mask, len))
			continue;
		if (cmp_mask(d_exe + ofs, op->v_msvc,
			     op->v_msvc_mask, len))
			continue;

		jo = je = 0;
		d_obj += ofs;
		d_exe += ofs;
		while (1)
		{
			for (; jo < len; jo++)
				if (op->v_masm_mask[jo] != 0xff)
					break;
			for (; je < len; je++)
				if (op->v_msvc_mask[je] != 0xff)
					break;

			if ((jo == len && je != len) || (jo != len && je == len)) {
				printf("invalid equiv_ops\n");
				return -1;
			}
			if (jo == len)
				return len + ofs - 1; // matched

			// var byte
			vo = d_obj[jo] & ~op->v_masm_mask[jo];
			ve = d_exe[je] & ~op->v_msvc_mask[je];
			if (op->cmp_rm && op->v_masm_mask[jo] == 0xc0) {
				vo2 = vo >> 3;
				vo &= 7;
				ve2 = ve & 7;
				ve >>= 3;
				if (vo != ve || vo2 != ve2)
					return -1;
			}
			else {
				if (vo != ve)
					return -1;
			}

			jo++;
			je++;
		}
	}

	return -1;
}

int main(int argc, char *argv[])
{
	FILE *f_obj, *f_exe;
	long text_ofs_obj, text_ofs_exe;
	long sztext_obj, sztext_exe, sztext_cmn;
	RELOC *relocs_obj, *relocs_exe;
	long reloc_cnt_obj, reloc_cnt_exe, reloc_cnt_cmn;
	unsigned int base = 0;
	uint8_t *d_obj, *d_exe;
	int retval = 1;
	int left;
	int ret;
	int i;

	if (argc != 3) {
		printf("usage:\n%s <obj> <exe>\n", argv[0]);
		return 1;
	}

	f_obj = fopen(argv[1], "r+b");
	if (f_obj == NULL) {
		fprintf(stderr, "%s", argv[1]);
		perror("");
		return 1;
	}

	f_exe = fopen(argv[2], "r");
	if (f_exe == NULL) {
		fprintf(stderr, "%s", argv[1]);
		perror("");
		return 1;
	}

	parse_headers(f_obj, &base, &text_ofs_obj, &d_obj, &sztext_obj,
		&relocs_obj, &reloc_cnt_obj);
	parse_headers(f_exe, &base, &text_ofs_exe, &d_exe, &sztext_exe,
		&relocs_exe, &reloc_cnt_exe);

	sztext_cmn = sztext_obj;
	if (sztext_cmn > sztext_exe)
		sztext_cmn = sztext_exe;
	reloc_cnt_cmn = reloc_cnt_obj;
	if (reloc_cnt_cmn > reloc_cnt_exe)
		reloc_cnt_cmn = reloc_cnt_exe;

	if (sztext_cmn == 0) {
		printf("bad .text size(s): %ld, %ld\n",
			sztext_obj, sztext_exe);
		return 1;
	}

	for (i = 0; i < reloc_cnt_obj; i++) // reloc_cnt_cmn
	{
		unsigned int a = relocs_obj[i].r_vaddr;
		//printf("%04x %08x\n", relocs_obj[i].r_type, a);

		switch (relocs_obj[i].r_type) {
		case 0x06:
			memset(d_obj + a, 0, 4);
			memset(d_exe + a, 0, 4);
			break;
		default:
			printf("unknown reloc %x @%08x\n",
				relocs_obj[i].r_type, base + a);
			return 1;
		}
	}

	for (i = 0; i < sztext_cmn; i++)
	{
		if (d_obj[i] == d_exe[i])
			continue;

		left = sztext_cmn - i;

		if (d_exe[i] == 0xcc) { // padding
			if (handle_pad(d_obj + i, d_exe + i, left))
				continue;
		}

		ret = check_equiv(d_obj + i, d_exe + i, left);
		if (ret >= 0) {
			i += ret;
			continue;
		}

		printf("%x: %02x vs %02x\n", base + i, d_obj[i], d_exe[i]);
		goto out;
	}

out:
	return retval;
}