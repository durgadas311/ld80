#include <stdio.h>
#include <stdlib.h>
#ifndef WINHACK
#include <unistd.h>
#endif
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>
#include "ld80.h"

int warn_extchain, debug;
static char *ofilename, *symfilename;
int fatalerror;
static int entry_jmp = -1;

void usage(void);
int setformat(char *name, int *format);
static void check_entry(char *name, int point);

struct loc rel_entry = {0};

int main(int argc,char **argv)
{
	int n;
	int c, type, align;
	int suppress_data = 0;
	int oformat = F_IHEX;
	FILE *ofile, *symfile=NULL;
	int symsize = 32768;
	int entry_point = -1;
	char *entry_name = NULL;
	int argc2 = 1;
	char **argv2;
	int abort = 0;
	int lib = 0;
	int symbol_table_required = 0, map_required = 0;
	char *common_name = "COMMON";
	char *optarg;
	char *tmp;
	unsigned char *rbits = NULL;

	init_section();

	/*
	 * argv is processed in two passes. This ensures that
	 * -o will be consumed before opening any input file.
	 * So ld80 will know what file is to delete in case
	 * of an error.
	 */
	argv2 = calloc_or_die(argc*2+1, sizeof(*argv2));
	argc2=1;

#define	REGULAR_OPTSTRING	"VlP:D:C:U:E:O:o:hcs:mS:W:"
#ifdef	DEBUG
#define	OPTSTRING		REGULAR_OPTSTRING "d"
#else
#define	OPTSTRING		REGULAR_OPTSTRING
#endif

	while ((c = optget (argc, argv, OPTSTRING, &optarg)) != -1) switch (c) {
	case 1:		/* Input file */
		argv2[argc2++] = optarg;	/* defer processing */
		break;
	case 'P':	/* Program location */
		argv2[argc2++] = "-P";
		argv2[argc2++] = optarg;	/* defer processing */
		break;
	case 'D':	/* Data location */
		argv2[argc2++] = "-D";
		argv2[argc2++] = optarg;	/* defer processing */
		break;
	case 'C':	/* Common location */
		argv2[argc2++] = "-C";
		argv2[argc2++] = optarg;	/* defer processing */
		break;
	case 'l':	/* Library specification */
		argv2[argc2++] = "-l";		/* defer processing */
		break;

	case 'U':	/* "Uncommon" segment */
		if (isspace(*optarg)) *optarg = '\0';	/* blank common */
		{ char *s; for (s=optarg; *s; s++) *s = toupper(*s); }
		mark_uncommon(optarg);
		break;
	case 'S':	/* Set symbol table size */
		n = atoi(optarg);
		if (n<=0) {
			fprintf(stderr,"ld80: symbol table size is invalid\n");
			abort = 1;
			break;
		}
		if (symsize<n) symsize = n;
		break;
	case 'o':	/* Output file */
		ofilename = optarg;
		tmp = strrchr(optarg, '.');
		if (tmp)
			setformat(tmp + 1, &oformat);
		break;
	case 'O':	/* Output format */
		if (!setformat(optarg, &oformat)) {
			usage();
			abort = 1;
		}
		break;
	case 'c':	/* Suppress data segments */
		suppress_data++;
		break;
	case 'W':	/* Warnings */
		if (!strcmp(optarg,"extchain")) warn_extchain++;
		else {
			usage();
			abort = 1;
		}
		break;
	case 'E':	/* Entry Point */
		if (optarg[0] >= '0' && optarg[0] <= '9') {
			entry_point = strtoul(optarg, NULL, 16);
			if (entry_point < 0 || entry_point > 0xffff) {
				die(E_USAGE,"ld80: Address %x is out of range\n", entry_point);
			}
		}
		else {
			entry_name = optarg;
		}
		break;
	case 's':	/* Symbol table */
		symfilename = optarg;
		symbol_table_required++;
		break;
	case 'm':	/* Segment map */
		map_required++;
		break;
#ifdef	DEBUG
	case 'd':	/* Debug level */
		debug++;
		break;
#endif
	case 'V':	/* Version */
		fprintf(stderr,"ld80 v%s\n",VERSION);
		abort = 1;
		break;
	default:
	case 'h':	/* Help */
		usage();
		abort = 1;
		break;
	} /* switch(c) */

	if (symbol_table_required || map_required) {
		if (!symfilename || !strcmp(symfilename,"-")) {
			symfilename = NULL;
			symfile = stdout;
		}
		else {
			symfile = fopen(symfilename,"w");
			if (symfile == NULL) die(E_USAGE,
				"ld80: Cannot open symbol file %s: %s\n",
				symfilename, strerror(errno));
				// use sys_errlist[errno] if you don't have strerror()

		}
	}
	if (abort) die(E_USAGE,"");

	if (oformat == F_ABS) {
		set_base_address(T_CODE, "", 0x2280, 0);
	} else if (oformat == F_PIC) {
		set_base_address(T_CODE, "", 0x0000, 0);
	}

	/*
	 * Start processing object files.
	 */
	init_symbol(symsize);

	if (IS_CPM(oformat)) {
		struct object_item itm;

		/* TODO: prevent overrides from commandline args */
		int base = IS_CPM0100(oformat) ? 0x0100 : 0x0000;
		set_base_address(T_CODE, "", base, 0);
		entry_jmp = base;

		/* These may need to be deleted later... */
		itm.type = T_RELOCATABLE|T_SPECIAL;
		itm.v.special.control = C_PROG_SIZE;
		strncpy((char *)itm.v.special.B_name, "<ld80>", NAMELEN);
		itm.v.special.A_value = 3;
		itm.v.special.A_t = T_CODE;
		add_item(&itm, "<ld80>");
		itm.type = T_ABSOLUTE;
		itm.v.absolute_byte = 0xc3;
		add_item(&itm, "<ld80>");
		itm.type = T_RELOCATABLE|T_CODE;
		itm.v.relative_word = -base;	/* filled-in later */
		add_item(&itm, "<ld80>");
	}

	optget_ind = 0;	/* make reinitialize optget() */
	while ((c = optget (argc2, argv2, "lD:P:C:", &optarg)) != -1) switch (c) {
	case 'l':	/* Library to search in */
		lib = 1;
		break;
	case 'C':	/* Common location */
		common_name = optarg;
		for (/*EMPTY*/; *optarg && *optarg!=','; optarg++)
			*optarg = toupper(*optarg);
		if (*optarg != ',') {
			usage();
			die(E_USAGE,"");
		}
		*optarg++ = '\0';
		if (isspace(*common_name))
			*common_name = '\0';	/* blank common */
		type = T_COMMON;
		/* FALL THROUGH */
	case 'D':	/* Data location */
		if (c == 'D') type = T_DATA;
		/* FALL THROUGH */
	case 'P':	/* Program location */
		if (c == 'P') type = T_CODE;
		if (*optarg == '%') {
			align = 1;
			optarg++;
		}
		else align = 0;
		n = strtoul(optarg, NULL, 16);
		if (0xffff<n)
			die(E_USAGE,"ld80: Address %x is out of range\n", n);
		set_base_address(type, common_name, n, align);
		break;
	case 1:		/* Input file */
		read_object_file(optarg, lib, oformat);
		lib = 0;
		break;
	}
	free(argv2);

	check_entry(entry_name, entry_point);

#ifdef	DEBUG
#define IFDEBUG(x)	if (debug) x
#else
#define	IFDEBUG(x)
#endif
	IFDEBUG( printf("\nRelocating sections\n"); )
	relocate_sections(oformat);
	IFDEBUG( dump_sections(); )

	IFDEBUG( printf("\nSetting symbol values\n"); )
	set_symbols();
	IFDEBUG( dump_symbols(); )

	if (IS_CPMRELO(oformat)) {
		rbits = calloc_or_die(1, 0x10000 / 8);
	} else if (oformat == F_PIC) {
		rbits = calloc_or_die(1, 0x10000); // overkill, but always enough
	}
	IFDEBUG( printf("\nSetting fixups\n"); )
	set_fixups(oformat, rbits);
	IFDEBUG( dump_sections(); )

	IFDEBUG( printf("\nResolving externals\n"); )
	resolve_externals();

	IFDEBUG( printf("\nProcessing nodes\n"); )
	process_nodes(oformat, rbits);
	IFDEBUG( dump_sections(); )

	IFDEBUG( printf("\nJoining sections\n"); )
	join_sections(suppress_data);
	IFDEBUG( dump_sections(); )

	if (map_required) print_map(symfile);
	if (symbol_table_required) print_symbol_table(symfile);

	if (ofilename) {
		char *write_mode = oformat == F_IHEX ? "w" : "wb";
		if ((ofile=fopen(ofilename,write_mode)) == NULL) die(E_USAGE,
			"ld80: Cannot open output file %s: %s\n",
			ofilename, strerror(errno));
			// use sys_errlist[errno] if you don't have strerror()
	}
	else die(E_USAGE, "ld80: No output file specified\n");

	if (entry_name) {
		struct symbol *entry;
		char *s;
		for (s=entry_name; *s; s++) *s = toupper(*s);
		entry = find_symbol(entry_name);
		if (!entry || entry->value == UNDEFINED)
			die(E_USAGE,"ld80: Entry point '%s' not found.\n", entry_name);

		entry_point = entry->value;
	} else if (entry_point < 0 && rel_entry.section != NULL) {
		entry_point = rel_entry.section->base + rel_entry.offset;
	}
	if (entry_jmp >= 0) {
		extern unsigned char *aseg;
		int addr = entry_jmp;
		if (aseg[addr] == 0xc3 && aseg[addr + 1] == 0x00 &&
				aseg[addr + 2] == 0x00) {
			aseg[addr + 1] = entry_point;
			aseg[addr + 2] = entry_point >> 8;
		} else {
			die(E_INPUT, "Failed to fixup JMP %04x: %04x %02x %02x %02x\n",
				entry_point, addr, aseg[addr],
				aseg[addr + 1], aseg[addr + 2]);
		}
	}

	do_out(ofile, oformat, entry_point, rbits);
	fclose(ofile);

	clear_symbol();
	die(fatalerror ? E_INPUT : E_SUCCESS, "");
}

static
void check_entry(char *name, int point)
{
	if (entry_jmp < 0) return;
	if (name || point >= 0) {
		/* specific entry point, keep JMP */
		return;
	}
	if (rel_entry.section != NULL) {
		/* an entry point was found in the object files */
		int entry = rel_entry.section->base + rel_entry.offset;
		if (entry > entry_jmp + 3) {
			/* non-trivial entry point, keep JMP */
			return;
		}
	}
	/* JMP ENTRY is not needed, or not possible */
	entry_jmp = -1;
	delete_section(T_CODE, "<ld80>");
}

int setformat(char *name, int *format)
{
	int known = 1;

	if (!strcmp(name, "ihex")) *format = F_IHEX;
	else if (!strcmp(name, "hex")) *format = F_IHEX;
	else if (!strcmp(name, "bin")) *format = F_BIN00;
	else if (!strcmp(name, "binff")) *format = F_BINFF;
	else if (!strcmp(name, "cmd")) *format = F_CMD;
	else if (!strcmp(name, "abs")) *format = F_ABS;
	else if (!strcmp(name, "pic")) *format = F_PIC;
	else if (!strcmp(name, "com")) *format = F_COM;
	else if (!strcmp(name, "prl")) *format = F_PRL;
	else if (!strcmp(name, "spr")) *format = F_SPR;
	else if (!strcmp(name, "bspr")) *format = F_BSPR;
	else known = 0;

	return known;
}

void *calloc_or_die(size_t nmemb, size_t size)
{
	void *retval = calloc(nmemb, size);
	if (retval==NULL) die(E_RESOURCE,"ld80: not enough memory\n");
	return retval;
}

void usage(void)
{
	fprintf(stderr,
"Usage:\n"
"ld80 [-O oformat] [-cmV] [-W warns] -o ofile [-s symfile] [-U name] ...\n"
"     [-S symsize] input ...\n"
"where oformat: ihex | hex | bin | binff | cmd | abs | com | prl | spr | bspr\n"
"        warns: extchain\n"
"        input: [-l] [-P address] [-D address] [-C name,address] [-E entry]... file\n"
	);
}

void die(int status, const char *format, ...)
{
	va_list arg;

	fflush(stdout);
	va_start(arg, format);
	vfprintf(stderr, format, arg);
	va_end(arg);

	if (status && ofilename) unlink(ofilename);
//	if (status && symfilename) unlink(symfilename);
	exit(status);
}
