/*
 * ELF object format
 *
 *  Copyright (C) 2003  Michael Urman
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND OTHER CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR OTHER CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <util.h>
/*@unused@*/ RCSID("$IdPath$");

/* Notes
 *
 * elf-objfmt uses the "linking" view of an ELF file:
 * ELF header, an optional program header table, several sections,
 * and a section header table
 *
 * The ELF header tells us some overall program information,
 *   where to find the PHT (if it exists) with phnum and phentsize, 
 *   and where to find the SHT with shnum and shentsize
 *
 * The PHT doesn't seem to be generated by NASM for elftest.asm
 *
 * The SHT
 *
 * Each Section is spatially disjoint, and has exactly one SHT entry.
 */

#define YASM_LIB_INTERNAL
#define YASM_BC_INTERNAL
#define YASM_EXPR_INTERNAL
#include <libyasm.h>

#include "elf.h"

typedef struct {
    FILE *f;
    elf_secthead *shead;
    yasm_section *sect;
    unsigned long sindex;
    unsigned long addr;
} elf_objfmt_output_info;

static unsigned int elf_objfmt_parse_scnum;	/* sect numbering in parser */
static elf_symtab_head* elf_symtab;	    /* symbol table of indexed syms */
static elf_strtab_head* elf_shstrtab;		/* section name strtab */
static elf_strtab_head* elf_strtab;		/* strtab entries */

yasm_objfmt yasm_elf_LTX_objfmt;
static /*@dependent@*/ yasm_arch *cur_arch;


static elf_symtab_entry *
elf_objfmt_symtab_append(yasm_symrec *sym, elf_symbol_binding bind,
			 elf_section_index sectidx, yasm_expr *size)
{
    elf_strtab_entry *name = elf_strtab_append_str(elf_strtab,
						   yasm_symrec_get_name(sym));
    elf_symtab_entry *entry = elf_symtab_entry_new(name, sym);
    elf_symtab_append_entry(elf_symtab, entry);

    elf_symtab_set_nonzero(entry, NULL, sectidx, bind, STT_NOTYPE, size, 00);
    yasm_symrec_set_of_data(sym, &yasm_elf_LTX_objfmt, entry);

    return entry;
}

static int
elf_objfmt_append_local_sym(yasm_symrec *sym, /*@unused@*/ /*@null@*/ void *d)
{
    int local_names = *(int *)d;
    elf_symtab_entry *entry;
    elf_address value=0;
    yasm_section *sect=NULL;
    yasm_bytecode *precbc=NULL;

    if (!yasm_symrec_get_of_data(sym)) {
	int is_sect = 0;
	if (!yasm_symrec_get_label(sym, &sect, &precbc))
	    return 1;
	is_sect = strcmp(yasm_symrec_get_name(sym), yasm_section_get_name(sect))==0;

	/* neither sections nor locals (except when debugging) need names */
	entry = elf_symtab_insert_local_sym(elf_symtab,
		    local_names && !is_sect ? elf_strtab : NULL, sym);
	elf_symtab_set_nonzero(entry, sect, 0, STB_LOCAL,
			       is_sect ? STT_SECTION : STT_NOTYPE, NULL, 0);
	yasm_symrec_set_of_data(sym, &yasm_elf_LTX_objfmt, entry);

	if (is_sect)
	    return 1;
    }
    else {
	if (!yasm_symrec_get_label(sym, &sect, &precbc))
	    return 1;
    }

    entry = yasm_symrec_get_of_data(sym);
    if (precbc)
	value = precbc->offset + precbc->len;
    elf_symtab_set_nonzero(entry, sect, 0, 0, 0, NULL, value);

    return 1;
}

static void
elf_objfmt_initialize(const char *in_filename,
		       /*@unused@*/ const char *obj_filename,
		       /*@unused@*/ yasm_dbgfmt *df, yasm_arch *a)
{
    yasm_symrec *filesym;
    elf_symtab_entry *entry;

    cur_arch = a;
    elf_objfmt_parse_scnum = 4;    /* section numbering starts at 0;
				      4 predefined sections. */
    elf_shstrtab = elf_strtab_new();
    elf_strtab = elf_strtab_new();
    elf_symtab = elf_symtab_new();

    filesym = yasm_symrec_define_label(".file", NULL, NULL, 0, 0);
    entry = elf_symtab_entry_new(
	elf_strtab_append_str(elf_strtab, in_filename), filesym);
    yasm_symrec_set_of_data(filesym, &yasm_elf_LTX_objfmt, entry);
    elf_symtab_set_nonzero(entry, NULL, SHN_ABS, STB_LOCAL, STT_FILE, NULL, 0);
    elf_symtab_append_entry(elf_symtab, entry);

}

static long
elf_objfmt_output_align(FILE *f, unsigned int align)
{
    long pos;
    unsigned long delta;
    if ((align & (align-1)) != 0)
	yasm_internal_error("requested alignment not a power of two");

    pos = ftell(f);
    if (pos == -1) {
	yasm__error(0, N_("could not get file position on output file"));
	return -1;
    }
    delta = align - (pos & (align-1)); 
    if (delta != align) {
	pos += delta;
	if (fseek(f, pos, SEEK_SET) < 0) {
	    yasm__error(0, N_("could not set file position on output file"));
	    return -1;
	}
    }
    return pos;
}

/* PASS1 */
static int
elf_objfmt_output_expr(yasm_expr **ep, unsigned char *buf, size_t destsize,
			size_t valsize, int shift, unsigned long offset,
			/*@observer@*/ const yasm_section *sect,
			yasm_bytecode *bc, int rel, int warn,
			/*@null@*/ void *d)
{
    /*@null@*/ elf_objfmt_output_info *info = (elf_objfmt_output_info *)d;
    /*@dependent@*/ /*@null@*/ const yasm_intnum *intn;
    /*@dependent@*/ /*@null@*/ const yasm_floatnum *flt;
    /*@dependent@*/ /*@null@*/ yasm_symrec *sym;

    if (info == NULL)
	yasm_internal_error("null info struct");

    *ep = yasm_expr_simplify(*ep, yasm_common_calc_bc_dist);

    /* Handle floating point expressions */
    flt = yasm_expr_get_floatnum(ep);
    if (flt) {
	if (shift < 0)
	    yasm_internal_error(N_("attempting to negative shift a float"));
	return cur_arch->floatnum_tobytes(flt, buf, destsize, valsize,
					  (unsigned int)shift, warn, bc->line);
    }

    /* Handle integer expressions, with relocation if necessary */
    sym = yasm_expr_extract_symrec(ep, yasm_common_calc_bc_dist);
    if (sym) {
	elf_reloc_entry *reloc;
	yasm_sym_vis vis;

	/* XXX: this can't be platform portable */
	if (valsize != 32) {
	    yasm__error(bc->line, N_("elf: invalid relocation size"));
	    return 1;
	}

	vis = yasm_symrec_get_visibility(sym);
	if (!(vis & (YASM_SYM_COMMON|YASM_SYM_EXTERN)))
	{
	    yasm_section *label_sect;
	    yasm_bytecode *label_precbc;
	    /* Local symbols need relocation to their section's start */
	    if (yasm_symrec_get_label(sym, &label_sect, &label_precbc)) {
		/*@null@*/ elf_secthead *sym_shead;
		sym_shead = yasm_section_get_of_data(label_sect);
		assert(sym_shead != NULL);
		sym = elf_secthead_get_sym(sym_shead);
	    }
	}

	if (rel) {
	    reloc = elf_reloc_entry_new(sym, bc->offset + offset, R_386_PC32);

	    /* Need to reference to start of section, so add $$ in. */
	    *ep = yasm_expr_new(YASM_EXPR_ADD, yasm_expr_expr(*ep),
		yasm_expr_sym(yasm_symrec_define_label("$$", info->sect,
						       NULL, 0, (*ep)->line)),
		(*ep)->line);
	    /* HELP: and this seems to have the desired effect. */
	    *ep = yasm_expr_new(YASM_EXPR_ADD, yasm_expr_expr(*ep),
		yasm_expr_int(yasm_intnum_new_uint(bc->offset + offset)),
		(*ep)->line);
	    *ep = yasm_expr_simplify(*ep, yasm_common_calc_bc_dist);
	} else
	    reloc = elf_reloc_entry_new(sym, bc->offset + offset, R_386_32);

	/* allocate .rel sections on a need-basis */
	if (elf_secthead_append_reloc(info->shead, reloc))
	    elf_objfmt_parse_scnum++;
    }

    intn = yasm_expr_get_intnum(ep, NULL);
    if (intn)
	return cur_arch->intnum_tobytes(intn, buf, destsize, valsize, shift,
					bc, rel, warn, bc->line);

    /* Check for complex float expressions */
    if (yasm_expr__contains(*ep, YASM_EXPR_FLOAT)) {
	yasm__error(bc->line, N_("floating point expression too complex"));
	return 1;
    }

    yasm__error(bc->line, N_("elf: relocation too complex"));
    return 1;
}

/* PASS1 */
static int
elf_objfmt_output_bytecode(yasm_bytecode *bc, /*@null@*/ void *d)
{
    /*@null@*/ elf_objfmt_output_info *info = (elf_objfmt_output_info *)d;
    unsigned char buf[256];
    /*@null@*/ /*@only@*/ unsigned char *bigbuf;
    unsigned long size = 256;
    unsigned long multiple;
    unsigned long i;
    int gap;

    if (info == NULL)
	yasm_internal_error("null info struct");

    bigbuf = yasm_bc_tobytes(bc, buf, &size, &multiple, &gap, info->sect,
			     info, elf_objfmt_output_expr, NULL);

    /* Don't bother doing anything else if size ended up being 0. */
    if (size == 0) {
	if (bigbuf)
	    yasm_xfree(bigbuf);
	return 0;
    }

    elf_secthead_add_size(info->shead, multiple * size);

    /* Warn that gaps are converted to 0 and write out the 0's. */
    if (gap) {
	unsigned long left;
	yasm__warning(YASM_WARN_GENERAL, bc->line,
	    N_("uninitialized space declared in code/data section: zeroing"));
	/* Write out in chunks */
	memset(buf, 0, 256);
	left = multiple*size;
	while (left > 256) {
	    fwrite(buf, 256, 1, info->f);
	    left -= 256;
	}
	fwrite(buf, left, 1, info->f);
    } else {
	/* Output multiple copies of buf (or bigbuf if non-NULL) to file */
	for (i=0; i<multiple; i++)
	    fwrite(bigbuf ? bigbuf : buf, (size_t)size, 1, info->f);
    }

    /* If bigbuf was allocated, free it */
    if (bigbuf)
	yasm_xfree(bigbuf);

    return 0;
}

/* PASS1 */
static int
elf_objfmt_output_section(yasm_section *sect, /*@null@*/ void *d)
{
    /*@null@*/ elf_objfmt_output_info *info = (elf_objfmt_output_info *)d;
    /*@dependent@*/ /*@null@*/ elf_secthead *shead;
    long pos;
    char *relname;
    unsigned int relname_len;
    const char *sectname;

    /* Don't output absolute sections into the section table */
    if (yasm_section_is_absolute(sect))
	return 0;

    if (info == NULL)
	yasm_internal_error("null info struct");
    shead = yasm_section_get_of_data(sect);
    if (shead == NULL)
	yasm_internal_error("no section header attached to section");

    /*elf_secthead_set_addr(shead, info->addr);*/

    /* don't output header-only sections */
    if ((elf_secthead_get_type(shead) & SHT_NOBITS) == SHT_NOBITS)
    {
	yasm_bytecode *last = yasm_bcs_last(yasm_section_get_bytecodes(sect));
	if (last)
	    elf_secthead_add_size(shead, last->offset + last->len);
	elf_secthead_set_index(shead, ++info->sindex);
	return 0;
    }

    /* skip empty sections */
    if (!yasm_bcs_last(yasm_section_get_bytecodes(sect))) {
	return 0;
    }

    if ((pos = ftell(info->f)) == -1)
	yasm__error(0, N_("couldn't read position on output stream"));
    pos = elf_secthead_set_file_offset(shead, pos);
    if (fseek(info->f, pos, SEEK_SET) < 0)
	yasm__error(0, N_("couldn't seek on output stream"));

    info->sect = sect;
    info->shead = shead;
    yasm_bcs_traverse(yasm_section_get_bytecodes(sect), info,
		      elf_objfmt_output_bytecode);

    /* Empty?  Go on to next section */
    if (elf_secthead_get_size(shead) == 0)
	return 0;

    info->addr += elf_secthead_get_size(shead);
    elf_secthead_set_index(shead, ++info->sindex);

    /* No relocations to output?  Go on to next section */
    if (elf_secthead_write_relocs_to_file(info->f, shead) == 0)
	return 0;
    elf_secthead_set_rel_index(shead, ++info->sindex);

    /* name the relocation section .rel.foo */
    sectname = yasm_section_get_name(sect);
    relname_len = strlen(sectname)+5;
    relname = yasm_xmalloc(relname_len);
    snprintf(relname, relname_len, ".rel%s", sectname);
    elf_secthead_set_rel_name(shead,
			      elf_strtab_append_str(elf_shstrtab, relname));
    yasm_xfree(relname);

    return 0;
}


/* PASS1 */
static int
elf_objfmt_output_secthead(yasm_section *sect, /*@null@*/ void *d)
{
    /*@null@*/ elf_objfmt_output_info *info = (elf_objfmt_output_info *)d;
    /*@dependent@*/ /*@null@*/ elf_secthead *shead;

    /* Don't output absolute sections into the section table */
    if (yasm_section_is_absolute(sect))
	return 0;

    if (info == NULL)
	yasm_internal_error("null info struct");
    shead = yasm_section_get_of_data(sect);
    if (shead == NULL)
	yasm_internal_error("no section header attached to section");

    if(elf_secthead_write_to_file(info->f, shead, info->sindex+1))
	info->sindex++;

    /* output strtab headers here? */

    /* relocation entries for .foo are stored in section .rel.foo */
    if(elf_secthead_write_rel_to_file(info->f, 3, shead, info->sindex+1))
	info->sindex++;

    return 0;
}

static void
elf_objfmt_output(FILE *f, yasm_sectionhead *sections, int all_syms)
{
    elf_objfmt_output_info info;
    long pos;
    unsigned long elf_shead_addr;
    elf_secthead *esdn;
    unsigned long elf_strtab_offset, elf_shstrtab_offset, elf_symtab_offset;
    unsigned long elf_strtab_size, elf_shstrtab_size, elf_symtab_size;
    elf_strtab_entry *elf_strtab_name, *elf_shstrtab_name, *elf_symtab_name;
    unsigned long elf_symtab_nlocal;

    info.f = f;
    info.addr = 0;

    /* Allocate space for Ehdr by seeking forward */
    if (fseek(f, (long)(EHDR_SIZE), SEEK_SET) < 0) {
	yasm__error(0, N_("could not seek on output file"));
	return;
    }

    /* add all (local) syms to symtab because relocation needs a symtab index
     * if all_syms, register them by name.  if not, use strtab entry 0 */
    yasm_symrec_traverse((void *)&all_syms, elf_objfmt_append_local_sym);
    elf_symtab_nlocal = elf_symtab_assign_indices(elf_symtab);
    
    /* output known sections - includes reloc sections which aren't in yasm's
     * list.  Assign indices as we go. */
    info.sindex = 3;
    info.addr = 0;
    if (yasm_sections_traverse(sections, &info, elf_objfmt_output_section))
	return;

    /* add final sections to the shstrtab */
    elf_strtab_name = elf_strtab_append_str(elf_shstrtab, ".strtab");
    elf_symtab_name = elf_strtab_append_str(elf_shstrtab, ".symtab");
    elf_shstrtab_name = elf_strtab_append_str(elf_shstrtab, ".shstrtab");

    /* output .shstrtab */
    if ((pos = elf_objfmt_output_align(f, 4)) == -1)
	return;
    elf_shstrtab_offset = (unsigned long) pos;
    elf_shstrtab_size = elf_strtab_output_to_file(f, elf_shstrtab);

    /* output .strtab */
    if ((pos = elf_objfmt_output_align(f, 4)) == -1)
	return;
    elf_strtab_offset = (unsigned long) pos;
    elf_strtab_size = elf_strtab_output_to_file(f, elf_strtab);

    /* output .symtab - last section so all others have indexes */
    if ((pos = elf_objfmt_output_align(f, 4)) == -1)
	return;
    elf_symtab_offset = (unsigned long) pos;
    elf_symtab_size = elf_symtab_write_to_file(f, elf_symtab);

    /* output section header table */
    if ((pos = elf_objfmt_output_align(f, 16)) == -1)
	return;
    elf_shead_addr = (unsigned long) pos;

    /* output dummy section header - 0 */
    info.sindex = 0;


    esdn = elf_secthead_new(NULL, SHT_NULL, 0, 0, 0, 0);
    elf_secthead_write_to_file(f, esdn, 0);
    elf_secthead_delete(esdn);

    esdn = elf_secthead_new(elf_shstrtab_name, SHT_STRTAB, 0, 1,
			    elf_shstrtab_offset, elf_shstrtab_size);
    elf_secthead_write_to_file(f, esdn, 1);
    elf_secthead_delete(esdn);

    esdn = elf_secthead_new(elf_strtab_name, SHT_STRTAB, 0, 2,
			    elf_strtab_offset, elf_strtab_size);
    elf_secthead_write_to_file(f, esdn, 2);
    elf_secthead_delete(esdn);

    esdn = elf_secthead_new(elf_symtab_name, SHT_SYMTAB, 0, 3,
			    elf_symtab_offset, elf_symtab_size);
    elf_secthead_set_info(esdn, elf_symtab_nlocal);
    elf_secthead_set_link(esdn, 2);	/* for .strtab, which is index 2 */
    elf_secthead_write_to_file(f, esdn, 3);
    elf_secthead_delete(esdn);

    info.sindex = 3;
    /* output remaining section headers */
    yasm_sections_traverse(sections, &info, elf_objfmt_output_secthead);

    /* output Ehdr */
    if (fseek(f, 0, SEEK_SET) < 0) {
	yasm__error(0, N_("could not seek on output file"));
	return;
    }

    elf_proghead_write_to_file(f, elf_shead_addr, info.sindex+1, 1);
}

static void
elf_objfmt_cleanup(void)
{
    elf_symtab_delete(elf_symtab);
    elf_strtab_delete(elf_shstrtab);
    elf_strtab_delete(elf_strtab);
}

static /*@observer@*/ /*@null@*/ yasm_section *
elf_objfmt_sections_switch(yasm_sectionhead *headp,
			    yasm_valparamhead *valparams,
			    /*@unused@*/ /*@null@*/
			    yasm_valparamhead *objext_valparams,
			    unsigned long lindex)
{
    yasm_valparam *vp = yasm_vps_first(valparams);
    yasm_section *retval;
    int isnew;
    unsigned long type = SHT_PROGBITS;
    unsigned long flags = SHF_ALLOC;
    unsigned long align = 4;
    int flags_override = 0;
    char *sectname;
    int resonly = 0;
    static const struct {
	const char *name;
	unsigned long flags;
    } flagquals[] = {
	{ "alloc",	SHF_ALLOC },
	{ "exec",	SHF_EXECINSTR },
	{ "write",	SHF_WRITE },
	/*{ "progbits",	SHT_PROGBITS },*/
	/*{ "align",	0 } */
    };

    if (!vp || vp->param || !vp->val)
	return NULL;

    sectname = vp->val;

    if (strcmp(sectname, ".bss") == 0) {
	type = SHT_NOBITS;
	flags = SHF_ALLOC + SHF_WRITE;
	resonly = 1;
    } else if (strcmp(sectname, ".data") == 0) {
	type = SHT_PROGBITS;
	flags = SHF_ALLOC + SHF_WRITE;
    } else if (strcmp(sectname, ".rodata") == 0) {
	type = SHT_PROGBITS;
	flags = SHF_ALLOC;
    } else if (strcmp(sectname, ".text") == 0) {
	align = 16;
	type = SHT_PROGBITS;
	flags = SHF_ALLOC + SHF_EXECINSTR;
    } else {
	/* Default to code */
	align = 1;
    }

    while ((vp = yasm_vps_next(vp))) {
	size_t i;
	int match;

	match = 0;
	for (i=0; i<NELEMS(flagquals) && !match; i++) {
	    if (yasm__strcasecmp(vp->val, flagquals[i].name) == 0) {
		flags_override = 1;
		match = 1;
		flags |= flagquals[i].flags;
	    }
	    else if (yasm__strcasecmp(vp->val+2, flagquals[i].name) == 0
		  && yasm__strncasecmp(vp->val, "no", 2) == 0) {
		flags &= ~flagquals[i].flags;
		flags_override = 1;
		match = 1;
	    }
	}

	if (match)
	    ;
	else if (yasm__strcasecmp(vp->val, "progbits") == 0) {
	    type |= SHT_PROGBITS;
	}
	else if (yasm__strcasecmp(vp->val, "noprogbits") == 0) {
	    type &= ~SHT_PROGBITS;
	}
	else if (yasm__strcasecmp(vp->val, "align") == 0 && vp->param) {
	    if (0 /* win32 */) {
		/*@dependent@*/ /*@null@*/ const yasm_intnum *align_inum;
		unsigned long addralign;

		align_inum = yasm_expr_get_intnum(&vp->param, NULL);
		if (!align_inum) {
		    yasm__error(lindex,
				N_("argument to `%s' is not a power of two"),
				vp->val);
		    return NULL;
		}
		addralign = yasm_intnum_get_uint(align_inum);

		/* Alignments must be a power of two. */
		if ((addralign & (addralign - 1)) != 0) {
		    yasm__error(lindex,
				N_("argument to `%s' is not a power of two"),
				vp->val);
		    return NULL;
		}

		/* Convert alignment into flags setting */
		align = addralign;
	    } 
	} else
	    yasm__warning(YASM_WARN_GENERAL, lindex,
			  N_("Unrecognized qualifier `%s'"), vp->val);
    }

    retval = yasm_sections_switch_general(headp, sectname, 0, resonly, &isnew,
					  lindex);

    if (isnew) {
	elf_secthead *esd;
	yasm_symrec *sym;
	elf_strtab_entry *name = elf_strtab_append_str(elf_shstrtab, sectname);

	esd = elf_secthead_new(name, type, flags,
			       elf_objfmt_parse_scnum++, 0, 0);
	if (align) elf_secthead_set_align(esd, align);
	yasm_section_set_of_data(retval, &yasm_elf_LTX_objfmt, esd);
	sym = yasm_symrec_define_label(sectname, retval, (yasm_bytecode *)NULL,
				       1, lindex);

	elf_secthead_set_sym(esd, sym);
    } else if (flags_override)
	yasm__warning(YASM_WARN_GENERAL, lindex,
		      N_("section flags ignored on section redeclaration"));
    return retval;
}

static void
elf_objfmt_section_data_delete(/*@only@*/ void *data)
{
    elf_secthead_delete((elf_secthead *)data);
}

static void
elf_objfmt_section_data_print(FILE *f, int indent_level, void *data)
{
    elf_secthead_print(f, indent_level, (elf_secthead *)data);
}

static void
elf_objfmt_extglob_declare(yasm_symrec *sym, /*@unused@*/
			    /*@null@*/ yasm_valparamhead *objext_valparams,
			    /*@unused@*/ unsigned long lindex)
{
    elf_objfmt_symtab_append(sym, STB_GLOBAL, SHN_UNDEF, NULL);
}

static void
elf_objfmt_common_declare(yasm_symrec *sym, /*@only@*/ yasm_expr *size,
			   /*@unused@*/ /*@null@*/
			   yasm_valparamhead *objext_valparams,
			   /*@unused@*/ unsigned long lindex)
{
    elf_objfmt_symtab_append(sym, STB_GLOBAL, SHN_COMMON, size);
}

static void
elf_objfmt_symrec_data_delete(/*@only@*/ void *data)
{
    /* do nothing, as this stuff is in the symtab anyway...  this speaks of bad
     * design/use or this stuff, i fear */

    /* watch for double-free here ... */
    /*elf_symtab_entry_delete((elf_symtab_entry *)data);*/
}

static void
elf_objfmt_symrec_data_print(FILE *f, int indent_level, void *data)
{
    elf_symtab_entry_print(f, indent_level, (elf_symtab_entry *)data);
}

static int
elf_objfmt_directive(/*@unused@*/ const char *name,
		      /*@unused@*/ yasm_valparamhead *valparams,
		      /*@unused@*/ /*@null@*/
		      yasm_valparamhead *objext_valparams,
		      /*@unused@*/ yasm_sectionhead *headp,
		      /*@unused@*/ unsigned long lindex)
{
    return 1;	/* no objfmt directives */
}


/* Define valid debug formats to use with this object format */
static const char *elf_objfmt_dbgfmt_keywords[] = {
    "null",
    NULL
};

/* Define objfmt structure -- see objfmt.h for details */
yasm_objfmt yasm_elf_LTX_objfmt = {
    "ELF",
    "elf",
    "o",
    ".text",
    32,
    elf_objfmt_dbgfmt_keywords,
    "null",
    elf_objfmt_initialize,
    elf_objfmt_output,
    elf_objfmt_cleanup,
    elf_objfmt_sections_switch,
    elf_objfmt_section_data_delete,
    elf_objfmt_section_data_print,
    elf_objfmt_extglob_declare,
    elf_objfmt_extglob_declare,
    elf_objfmt_common_declare,
    elf_objfmt_symrec_data_delete,
    elf_objfmt_symrec_data_print,
    elf_objfmt_directive,
    NULL /*elf_objfmt_bc_objfmt_data_delete*/,
    NULL /*elf_objfmt_bc_objfmt_data_print*/
};
