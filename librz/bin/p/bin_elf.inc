// SPDX-FileCopyrightText: 2009-2021 nibble <nibble.ds@gmail.com>
// SPDX-FileCopyrightText: 2009-2021 pancake <pancake@nopcode.org>
// SPDX-License-Identifier: LGPL-3.0-only

#include <stdio.h>
#include <rz_types.h>
#include <rz_util.h>
#include <rz_lib.h>
#include <rz_bin.h>
#include <rz_io.h>
#include <rz_cons.h>
#include "../i/private.h"
#include "elf/elf.h"
#include <ht_uu.h>

static RzBinInfo *info(RzBinFile *bf);

//TODO: implement rz_bin_symbol_dup() and rz_bin_symbol_free ?

static int get_file_type(RzBinFile *bf) {
	struct Elf_(rz_bin_elf_obj_t) *obj = bf->o->bin_obj;
	char *type = Elf_(rz_bin_elf_get_file_type(obj));
	int res = type ? ((!strncmp(type, "CORE", 4)) ? RZ_BIN_TYPE_CORE : RZ_BIN_TYPE_DEFAULT) : -1;
	free(type);
	return res;
}

static char *regstate(RzBinFile *bf) {
	struct Elf_(rz_bin_elf_obj_t) *obj = bf->o->bin_obj;
	if (obj->ehdr.e_machine != EM_AARCH64 &&
		obj->ehdr.e_machine != EM_ARM &&
		obj->ehdr.e_machine != EM_386 &&
		obj->ehdr.e_machine != EM_X86_64) {
		eprintf("Cannot retrieve regstate on: %s (not yet supported)\n",
			Elf_(rz_bin_elf_get_machine_name)(obj));
		return NULL;
	}

	size_t sz = 0;
	const ut8 *regs = Elf_(rz_bin_elf_grab_regstate)(obj, &sz);
	char *hexregs = (regs && sz) ? rz_hex_bin2strdup(regs, sz) : NULL;

	return hexregs;
}

static void setimpord(ELFOBJ *eobj, ut32 ord, RzBinImport *ptr) {
	if (!eobj->imports_by_ord || ord >= eobj->imports_by_ord_size) {
		return;
	}
	rz_bin_import_free(eobj->imports_by_ord[ord]);
	eobj->imports_by_ord[ord] = rz_bin_import_clone(ptr);
}

static char *setphname(ut16 mach, Elf_(Word) ptyp) {
	// TODO to complete over time
	if (mach == EM_ARM) {
		if (ptyp == SHT_ARM_EXIDX) {
			return strdup("EXIDX");
		}
	} else if (mach == EM_MIPS) {
		if (ptyp == PT_MIPS_ABIFLAGS) {
			return strdup("ABIFLAGS");
		} else if (ptyp == PT_MIPS_REGINFO) {
			return strdup("REGINFO");
		}
	}

	return strdup("UNKNOWN");
}

static Sdb *get_sdb(RzBinFile *bf) {
	RzBinObject *o = bf->o;
	if (o && o->bin_obj) {
		struct Elf_(rz_bin_elf_obj_t) *bin = (struct Elf_(rz_bin_elf_obj_t) *)o->bin_obj;
		return bin->kv;
	}
	return NULL;
}

static bool load_buffer(RzBinFile *bf, void **bin_obj, RzBuffer *buf, ut64 loadaddr, Sdb *sdb) {
	struct Elf_(rz_bin_elf_obj_t) *res = Elf_(rz_bin_elf_new_buf)(buf, bf->rbin->verbose);
	if (res) {
		//	sdb_ns_set (sdb, "info", res->kv);
		*bin_obj = res;
		return true;
	}
	return false;
}

static void destroy(RzBinFile *bf) {
	int i;
	ELFOBJ *eobj = bf->o->bin_obj;
	if (eobj && eobj->imports_by_ord) {
		for (i = 0; i < eobj->imports_by_ord_size; i++) {
			RzBinImport *imp = eobj->imports_by_ord[i];
			if (imp) {
				free(imp->name);
				free(imp);
				eobj->imports_by_ord[i] = NULL;
			}
		}
		RZ_FREE(eobj->imports_by_ord);
	}
	Elf_(rz_bin_elf_free)((struct Elf_(rz_bin_elf_obj_t) *)bf->o->bin_obj);
}

static ut64 baddr(RzBinFile *bf) {
	return Elf_(rz_bin_elf_get_baddr)(bf->o->bin_obj);
}

static ut64 boffset(RzBinFile *bf) {
	return Elf_(rz_bin_elf_get_boffset)(bf->o->bin_obj);
}

static RzBinAddr *binsym(RzBinFile *bf, RzBinSpecialSymbol sym) {
	struct Elf_(rz_bin_elf_obj_t) *obj = bf->o->bin_obj;
	RzBinAddr *ret = NULL;
	ut64 addr = 0LL;

	switch (sym) {
	case RZ_BIN_SPECIAL_SYMBOL_ENTRY:
		addr = Elf_(rz_bin_elf_get_entry_offset)(bf->o->bin_obj);
		break;
	case RZ_BIN_SPECIAL_SYMBOL_MAIN:
		addr = Elf_(rz_bin_elf_get_main_offset)(bf->o->bin_obj);
		break;
	case RZ_BIN_SPECIAL_SYMBOL_INIT:
		addr = Elf_(rz_bin_elf_get_init_offset)(bf->o->bin_obj);
		break;
	case RZ_BIN_SPECIAL_SYMBOL_FINI:
		addr = Elf_(rz_bin_elf_get_fini_offset)(bf->o->bin_obj);
		break;
	default:
		break;
	}
	if (addr && addr != UT64_MAX && (ret = RZ_NEW0(RzBinAddr))) {
		struct Elf_(rz_bin_elf_obj_t) *bin = bf->o->bin_obj;
		bool is_arm = bin->ehdr.e_machine == EM_ARM;
		ret->paddr = addr;
		ret->vaddr = Elf_(rz_bin_elf_p2v)(obj, addr);
		if (is_arm && addr & 1) {
			ret->bits = 16;
			ret->vaddr--;
			ret->paddr--;
		}
	}
	return ret;
}

static bool is_wordable_section(const char *name) {
	if (!strcmp(name, ".init_array")) {
		return true;
	}
	if (!strcmp(name, ".fini_array")) {
		return true;
	}
	if (!strcmp(name, ".data.rel.ro")) {
		return true;
	}
	if (!strcmp(name, ".dynamic")) {
		return true;
	}
	if (!strcmp(name, ".got")) {
		return true;
	}
	if (strstr(name, ".rela.")) {
		return true;
	}
	return false;
}

static RzBinElfNoteFile *note_file_for_load_segment(struct Elf_(rz_bin_elf_obj_t) * obj, Elf_(Phdr) * phdr) {
	if (!obj->note_segments) {
		return false;
	}
	RzBinElfNoteSegment *seg;
	RzListIter *it;
	rz_list_foreach (obj->note_segments, it, seg) {
		for (size_t i = 0; i < seg->notes_count; i++) {
			RzBinElfNote *note = &seg->notes[i];
			if (note->type != NT_FILE) {
				continue;
			}
			for (size_t j = 0; j < note->file.files_count; j++) {
				RzBinElfNoteFile *f = &note->file.files[j];
				if (f->start_vaddr == phdr->p_vaddr) {
					return f;
				}
			}
		}
	}
	return NULL;
}

static RzList *sections(RzBinFile *bf) {
	struct Elf_(rz_bin_elf_obj_t) *obj = (bf && bf->o) ? bf->o->bin_obj : NULL;
	struct rz_bin_elf_section_t *section = NULL;
	int i, num, found_load = 0;
	Elf_(Phdr) *phdr = NULL;
	RzBinSection *ptr = NULL;
	RzList *ret = NULL;

	if (!obj || !(ret = rz_list_newf((RzListFree)rz_bin_section_free))) {
		return NULL;
	}

	//there is not leak in section since they are cached by elf.c
	//and freed within Elf_(rz_bin_elf_free)
	if ((section = Elf_(rz_bin_elf_get_sections)(obj))) {
		for (i = 0; !section[i].last; i++) {
			if (!(ptr = RZ_NEW0(RzBinSection))) {
				break;
			}
			ptr->name = strdup((char *)section[i].name);
			if (strstr(ptr->name, "data") && !strstr(ptr->name, "rel") && !strstr(ptr->name, "pydata")) {
				ptr->is_data = true;
			} else if (!strcmp(ptr->name, "C")) {
				ptr->is_data = true;
			}
			if (is_wordable_section(ptr->name)) {
				ptr->format = rz_str_newf("Cd %d[%" PFMT64d "]",
					RZ_BIN_ELF_WORDSIZE, section[i].size / RZ_BIN_ELF_WORDSIZE);
			}
			ptr->size = section[i].type != SHT_NOBITS ? section[i].size : 0;
			ptr->vsize = section[i].size;
			ptr->paddr = section[i].offset;
			ptr->vaddr = section[i].rva;
			ptr->type = section[i].type;
			ptr->flags = section[i].flags;
			ptr->add = !obj->phdr; // Load sections if there is no PHDR
			ptr->perm = 0;
			if (RZ_BIN_ELF_SCN_IS_EXECUTABLE(section[i].flags)) {
				ptr->perm |= RZ_PERM_X;
			}
			if (RZ_BIN_ELF_SCN_IS_WRITABLE(section[i].flags)) {
				ptr->perm |= RZ_PERM_W;
			}
			if (RZ_BIN_ELF_SCN_IS_READABLE(section[i].flags)) {
				ptr->perm |= RZ_PERM_R;
			}
			rz_list_append(ret, ptr);
		}
	}

	// program headers is another section
	ut16 mach = obj->ehdr.e_machine;
	num = obj->ehdr.e_phnum;
	phdr = obj->phdr;
	ut64 core_sp = Elf_(rz_bin_elf_get_sp_val)(obj);
	if (phdr) {
		int n = 0;
		for (i = 0; i < num; i++) {
			if (!(ptr = RZ_NEW0(RzBinSection))) {
				return ret;
			}
			ptr->add = false;
			ptr->size = phdr[i].p_filesz;
			ptr->vsize = phdr[i].p_memsz;
			ptr->paddr = phdr[i].p_offset;
			ptr->vaddr = phdr[i].p_vaddr;
			ptr->perm = phdr[i].p_flags;
			ptr->is_segment = true;
			switch (phdr[i].p_type) {
			case PT_DYNAMIC:
				ptr->name = strdup("DYNAMIC");
				break;
			case PT_LOAD: {
				ptr->name = rz_str_newf("LOAD%d", n++);
				ptr->perm |= RZ_PERM_R;
				found_load = 1;
				ptr->add = true;
				if (core_sp != UT64_MAX && core_sp >= phdr[i].p_vaddr && core_sp < phdr[i].p_vaddr + phdr[i].p_memsz) {
					ptr->map_name = strdup("[stack]");
				} else {
					RzBinElfNoteFile *nf = note_file_for_load_segment(obj, &phdr[i]);
					if (nf && nf->file) {
						ptr->map_name = strdup(nf->file);
					}
				}
				break;
			}
			case PT_INTERP:
				ptr->name = strdup("INTERP");
				break;
			case PT_GNU_STACK:
				ptr->name = strdup("GNU_STACK");
				break;
			case PT_GNU_RELRO:
				ptr->name = strdup("GNU_RELRO");
				break;
			case PT_GNU_EH_FRAME:
				ptr->name = strdup("GNU_EH_FRAME");
				break;
			case PT_PHDR:
				ptr->name = strdup("PHDR");
				break;
			case PT_TLS:
				ptr->name = strdup("TLS");
				break;
			case PT_NOTE:
				ptr->name = strdup("NOTE");
				break;
			case PT_OPENBSD_RANDOMIZE:
				ptr->name = strdup("OPENBSD_RANDOMIZE");
				break;
			case PT_OPENBSD_WXNEEDED:
				ptr->name = strdup("OPENBSD_WXNEEDED");
				break;
			case PT_OPENBSD_BOOTDATA:
				ptr->name = strdup("OPENBSD_BOOTDATA");
				break;
			default:
				if (ptr->size == 0 && ptr->vsize == 0) {
					ptr->name = strdup("NONE");
				} else {
					ptr->name = setphname(mach, phdr[i].p_type);
				}
				break;
			}
			rz_list_append(ret, ptr);
		}
	}

	if (rz_list_empty(ret)) {
		if (!bf->size) {
			struct Elf_(rz_bin_elf_obj_t) *bin = bf->o->bin_obj;
			bf->size = bin ? bin->size : 0x9999;
		}
		if (found_load == 0) {
			if (!(ptr = RZ_NEW0(RzBinSection))) {
				return ret;
			}
			ptr->name = strdup("uphdr");
			ptr->size = bf->size;
			ptr->vsize = bf->size;
			ptr->paddr = 0;
			ptr->vaddr = 0x10000;
			ptr->add = true;
			ptr->perm = RZ_PERM_RWX;
			rz_list_append(ret, ptr);
		}
	}
	// add entry for ehdr
	ptr = RZ_NEW0(RzBinSection);
	if (ptr) {
		ut64 ehdr_size = sizeof(obj->ehdr);
		if (bf->size < ehdr_size) {
			ehdr_size = bf->size;
		}
		ptr->name = strdup("ehdr");
		ptr->paddr = 0;
		ptr->vaddr = obj->baddr;
		ptr->size = ehdr_size;
		ptr->vsize = ehdr_size;
		ptr->add = false;
		if (obj->ehdr.e_type == ET_REL) {
			ptr->add = true;
		}
		ptr->perm = RZ_PERM_RW;
		ptr->is_segment = true;
		rz_list_append(ret, ptr);
	}
	return ret;
}

static RzBinAddr *newEntry(RzBinFile *bf, ut64 hpaddr, ut64 hvaddr, ut64 vaddr, int type, int bits) {
	rz_return_val_if_fail(bf && bf->o && bf->o->bin_obj, NULL);

	struct Elf_(rz_bin_elf_obj_t) *obj = bf->o->bin_obj;
	RzBinAddr *ptr = RZ_NEW0(RzBinAddr);
	if (ptr) {
		ptr->paddr = Elf_(rz_bin_elf_v2p)(obj, vaddr);
		ptr->vaddr = vaddr;
		ptr->hpaddr = hpaddr;
		ptr->hvaddr = hvaddr;
		ptr->bits = bits;
		ptr->type = type;
		//realign due to thumb
		if (bits == 16 && ptr->vaddr & 1) {
			ptr->paddr--;
			ptr->vaddr--;
		}
	}
	return ptr;
}

static void process_constructors(RzBinFile *bf, RzList *ret, int bits) {
	RzList *secs = sections(bf);
	RzListIter *iter;
	RzBinSection *sec;
	int i, type;
	rz_list_foreach (secs, iter, sec) {
		type = -1;
		if (!strcmp(sec->name, ".fini_array")) {
			type = RZ_BIN_ENTRY_TYPE_FINI;
		} else if (!strcmp(sec->name, ".init_array")) {
			type = RZ_BIN_ENTRY_TYPE_INIT;
		} else if (!strcmp(sec->name, ".preinit_array")) {
			type = RZ_BIN_ENTRY_TYPE_PREINIT;
		}
		if (type != -1) {
			ut8 *buf = calloc(sec->size, 1);
			if (!buf) {
				continue;
			}
			(void)rz_buf_read_at(bf->buf, sec->paddr, buf, sec->size);
			if (bits == 32) {
				for (i = 0; (i + 3) < sec->size; i += 4) {
					ut32 addr32 = rz_read_le32(buf + i);
					if (addr32) {
						RzBinAddr *ba = newEntry(bf, sec->paddr + i, sec->vaddr + i,
							(ut64)addr32, type, bits);
						rz_list_append(ret, ba);
					}
				}
			} else {
				for (i = 0; (i + 7) < sec->size; i += 8) {
					ut64 addr64 = rz_read_le64(buf + i);
					if (addr64) {
						RzBinAddr *ba = newEntry(bf, sec->paddr + i, sec->vaddr + i,
							addr64, type, bits);
						rz_list_append(ret, ba);
					}
				}
			}
			free(buf);
		}
	}
	rz_list_free(secs);
}

static RzList *entries(RzBinFile *bf) {
	rz_return_val_if_fail(bf && bf->o && bf->o->bin_obj, NULL);

	RzBinAddr *ptr = NULL;
	struct rz_bin_elf_symbol_t *symbol;
	int i;

	struct Elf_(rz_bin_elf_obj_t) *obj = bf->o->bin_obj;
	RzList *ret = rz_list_newf((RzListFree)free);
	if (!ret) {
		return NULL;
	}
	ut64 pa = Elf_(rz_bin_elf_get_entry_offset)(obj);
	if (pa != UT64_MAX) {
		if (!(ptr = RZ_NEW0(RzBinAddr))) {
			return ret;
		}
		ptr->paddr = pa;
		ptr->vaddr = Elf_(rz_bin_elf_p2v)(obj, ptr->paddr);
		ptr->hpaddr = 0x18; // e_entry offset in ELF header
		ptr->hvaddr = UT64_MAX;

		if (ptr->vaddr != (ut64)obj->ehdr.e_entry && Elf_(rz_bin_elf_is_executable)(obj)) {
			eprintf("Cannot determine entrypoint, using 0x%08" PFMT64x ".\n", ptr->vaddr);
		}

		if (bf->o->sections) {
			RzListIter *iter;
			RzBinSection *section;
			rz_list_foreach_prev(bf->o->sections, iter, section) {
				if (!strcmp(section->name, "ehdr")) {
					ptr->hvaddr = section->vaddr + ptr->hpaddr;
					break;
				}
			}
		}
		if (ptr->hvaddr == UT64_MAX) {
			Elf_(rz_bin_elf_p2v_new)(obj, ptr->hpaddr);
		}

		if (obj->ehdr.e_machine == EM_ARM) {
			int bin_bits = Elf_(rz_bin_elf_get_bits)(obj);
			if (bin_bits != 64) {
				ptr->bits = 32;
				if (ptr->vaddr & 1) {
					ptr->vaddr--;
					ptr->bits = 16;
				}
				if (ptr->paddr & 1) {
					ptr->paddr--;
					ptr->bits = 16;
				}
			}
		}
		rz_list_append(ret, ptr);
	}

	// add entrypoint for jni libraries
	// NOTE: this is slow, we shouldnt find for java constructors here
	if (!(symbol = Elf_(rz_bin_elf_get_symbols)(obj))) {
		return ret;
	}
	for (i = 0; !symbol[i].last; i++) {
		if (!strncmp(symbol[i].name, "Java", 4)) {
			if (rz_str_endswith(symbol[i].name, "_init")) {
				if (!(ptr = RZ_NEW0(RzBinAddr))) {
					return ret;
				}
				ptr->paddr = symbol[i].offset;
				ptr->vaddr = Elf_(rz_bin_elf_p2v)(obj, ptr->paddr);
				ptr->hpaddr = UT64_MAX;
				ptr->type = RZ_BIN_ENTRY_TYPE_INIT;
				rz_list_append(ret, ptr);
				break;
			}
		}
	}
	int bin_bits = Elf_(rz_bin_elf_get_bits)(bf->o->bin_obj);
	process_constructors(bf, ret, bin_bits < 32 ? 32 : bin_bits);
	return ret;
}

static void insert_symbol(struct Elf_(rz_bin_elf_obj_t) * bin, RzBinSymbol *ptr, bool is_sht_null, RzList *ret) {
	// add it to the list of symbols only if it doesn't point to SHT_NULL
	if (is_sht_null) {
		rz_bin_symbol_free(ptr);
	} else {
		rz_list_append(ret, ptr);
	}
}

static RzList *symbols(RzBinFile *bf) {
	struct rz_bin_elf_symbol_t *symbol = NULL;
	RzBinSymbol *ptr = NULL;
	int i;

	rz_return_val_if_fail(bf && bf->o && bf->o->bin_obj, NULL);

	struct Elf_(rz_bin_elf_obj_t) *bin = bf->o->bin_obj;
	RzList *ret = rz_list_newf((RzListFree)rz_bin_symbol_free);
	if (!ret) {
		return NULL;
	}

	// traverse symbols
	if (!(symbol = Elf_(rz_bin_elf_get_symbols)(bin))) {
		return ret;
	}
	for (i = 0; !symbol[i].last; i++) {
		ptr = Elf_(_r_bin_elf_convert_symbol)(bin, &symbol[i], "%s");
		if (!ptr) {
			break;
		}
		insert_symbol(bin, ptr, symbol[i].is_sht_null, ret);
	}

	// traverse imports
	if (!(symbol = Elf_(rz_bin_elf_get_imports)(bin))) {
		return ret;
	}
	for (i = 0; !symbol[i].last; i++) {
		if (!symbol[i].size) {
			continue;
		}
		ptr = Elf_(_r_bin_elf_convert_symbol)(bin, &symbol[i], "%s");
		if (!ptr) {
			break;
		}
		ptr->is_imported = true;
		// special case where there is not entry in the plt for the import
		if (ptr->vaddr == UT32_MAX) {
			ptr->paddr = 0;
			ptr->vaddr = 0;
		}
		insert_symbol(bin, ptr, symbol[i].is_sht_null, ret);
	}
	return ret;
}

static RzList *imports(RzBinFile *bf) {
	RzBinElfSymbol *import = NULL;
	RzBinImport *ptr = NULL;
	int i;

	rz_return_val_if_fail(bf && bf->o, NULL);
	struct Elf_(rz_bin_elf_obj_t) *elf = bf->o->bin_obj;
	RzList *ret = rz_list_newf((RzListFree)rz_bin_import_free);
	if (!ret) {
		return NULL;
	}
	if (!(import = Elf_(rz_bin_elf_get_imports)(elf))) {
		rz_list_free(ret);
		return NULL;
	}
	for (i = 0; !import[i].last; i++) {
		if (!(ptr = RZ_NEW0(RzBinImport))) {
			break;
		}
		ptr->name = strdup(import[i].name);
		ptr->bind = import[i].bind;
		ptr->type = import[i].type;
		ptr->ordinal = import[i].ordinal;
		setimpord(elf, ptr->ordinal, ptr);
		rz_list_append(ret, ptr);
	}
	return ret;
}

static RzList *libs(RzBinFile *bf) {
	rz_return_val_if_fail(bf && bf->o && bf->o->bin_obj, NULL);

	RzList *ret = rz_list_newf(free);
	if (!ret) {
		return NULL;
	}
	struct rz_bin_elf_lib_t *libs = Elf_(rz_bin_elf_get_libs)(bf->o->bin_obj);
	if (libs) {
		int i;
		for (i = 0; !libs[i].last; i++) {
			char *ptr = strdup(libs[i].name);
			rz_list_append(ret, ptr);
		}
		free(libs);
	}
	return ret;
}

static RzBinReloc *reloc_convert(struct Elf_(rz_bin_elf_obj_t) * bin, RzBinElfReloc *rel, ut64 GOT) {
	rz_return_val_if_fail(bin && rel, NULL);

	ut64 B = bin->baddr;
	ut64 P = rel->rva; // rva has taken baddr into account
	RzBinReloc *r = RZ_NEW0(RzBinReloc);
	if (!r) {
		return NULL;
	}
	r->import = NULL;
	r->symbol = NULL;
	r->is_ifunc = false;
	r->addend = rel->addend;
	if (rel->sym) {
		if (rel->sym < bin->imports_by_ord_size && bin->imports_by_ord[rel->sym]) {
			r->import = bin->imports_by_ord[rel->sym];
		} else if (rel->sym < bin->symbols_by_ord_size && bin->symbols_by_ord[rel->sym]) {
			r->symbol = bin->symbols_by_ord[rel->sym];
		}
	}
	r->vaddr = rel->rva;
	r->paddr = rel->offset;

#define SET(T) \
	r->type = RZ_BIN_RELOC_##T; \
	r->additive = 0; \
	return r
#define ADD(T, A) \
	r->type = RZ_BIN_RELOC_##T; \
	r->addend += A; \
	r->additive = rel->rel_mode == DT_RELA; \
	return r

	switch (bin->ehdr.e_machine) {
	case EM_386:
		switch (rel->type) {
		case RZ_386_NONE: break; // malloc then free. meh. then again, there's no real world use for _NONE.
		case RZ_386_32: ADD(32, 0);
		case RZ_386_PC32: ADD(32, -P);
		case RZ_386_GLOB_DAT: SET(32);
		case RZ_386_JMP_SLOT: SET(32);
		case RZ_386_RELATIVE: ADD(32, B);
		case RZ_386_GOTOFF: ADD(32, -GOT);
		case RZ_386_GOTPC: ADD(32, GOT - P);
		case RZ_386_16: ADD(16, 0);
		case RZ_386_PC16: ADD(16, -P);
		case RZ_386_8: ADD(8, 0);
		case RZ_386_PC8: ADD(8, -P);
		case RZ_386_COPY: ADD(32, 0); // XXX: copy symbol at runtime
		case RZ_386_IRELATIVE: r->is_ifunc = true; SET(32);
		default: break;
		}
		break;
	case EM_X86_64:
		switch (rel->type) {
		case RZ_X86_64_NONE: break; // malloc then free. meh. then again, there's no real world use for _NONE.
		case RZ_X86_64_64: ADD(64, 0);
		case RZ_X86_64_PLT32: ADD(32, -P /* +L */);
		case RZ_X86_64_GOT32: ADD(32, GOT);
		case RZ_X86_64_PC32: ADD(32, -P);
		case RZ_X86_64_GLOB_DAT: r->vaddr -= rel->sto; SET(64);
		case RZ_X86_64_JUMP_SLOT: r->vaddr -= rel->sto; SET(64);
		case RZ_X86_64_RELATIVE: ADD(64, B);
		case RZ_X86_64_32: ADD(32, 0);
		case RZ_X86_64_32S: ADD(32, 0);
		case RZ_X86_64_16: ADD(16, 0);
		case RZ_X86_64_PC16: ADD(16, -P);
		case RZ_X86_64_8: ADD(8, 0);
		case RZ_X86_64_PC8: ADD(8, -P);
		case RZ_X86_64_GOTPCREL: ADD(64, GOT - P);
		case RZ_X86_64_COPY: ADD(64, 0); // XXX: copy symbol at runtime
		case RZ_X86_64_IRELATIVE: r->is_ifunc = true; SET(64);
		default: break;
		}
		break;
	case EM_ARM:
		switch (rel->type) {
		case RZ_ARM_NONE: break;
		case RZ_ARM_ABS32: ADD(32, 0);
		case RZ_ARM_REL32: ADD(32, -P);
		case RZ_ARM_ABS16: ADD(16, 0);
		case RZ_ARM_ABS8: ADD(8, 0);
		case RZ_ARM_SBREL32: ADD(32, -B);
		case RZ_ARM_GLOB_DAT: ADD(32, 0);
		case RZ_ARM_JUMP_SLOT: ADD(32, 0);
		case RZ_ARM_RELATIVE: ADD(32, B);
		case RZ_ARM_GOTOFF: ADD(32, -GOT);
		default: ADD(32, GOT); break; // reg relocations
		}
		break;
	case EM_RISCV:
		switch (rel->type) {
		case RZ_RISCV_NONE: break;
		case RZ_RISCV_JUMP_SLOT: ADD(64, 0);
		case RZ_RISCV_RELATIVE: ADD(64, B);
		default: ADD(64, GOT); break; // reg relocations
		}
		break;
	case EM_AARCH64:
		switch (rel->type) {
		case RZ_AARCH64_NONE: break;
		case RZ_AARCH64_ABS32: ADD(32, 0);
		case RZ_AARCH64_ABS16: ADD(16, 0);
		case RZ_AARCH64_GLOB_DAT: SET(64);
		case RZ_AARCH64_JUMP_SLOT: SET(64);
		case RZ_AARCH64_RELATIVE: ADD(64, B);
		default: break; // reg relocations
		}
		break;
	case EM_PPC:
		switch (rel->type) {
		case RZ_PPC_NONE: break;
		case RZ_PPC_GLOB_DAT: ADD(32, 0);
		case RZ_PPC_JMP_SLOT: ADD(32, 0);
		default:
			eprintf("unimplemented ELF/PPC reloc type %d\n", rel->type);
			break;
		}
		break;
	default: break;
	}

#undef SET
#undef ADD

	free(r);
	return NULL;
}

static RzList *relocs(RzBinFile *bf) {
	rz_return_val_if_fail(bf && bf->o && bf->o->bin_obj, NULL);
	RzList *ret = NULL;
	RzBinReloc *ptr = NULL;
	RzBinElfReloc *relocs = NULL;
	struct Elf_(rz_bin_elf_obj_t) *bin = bf->o->bin_obj;
	if (!(ret = rz_list_newf(free))) {
		return NULL;
	}
	/* FIXME: This is a _temporary_ fix/workaround to prevent a use-after-
	 * free detected by ASan that would corrupt the relocation names */
	rz_list_free(imports(bf));
	ut64 got_addr = Elf_(rz_bin_elf_get_section_addr)(bin, ".got");
	if (got_addr == UT64_MAX) {
		got_addr = Elf_(rz_bin_elf_get_section_addr)(bin, ".got.plt");
		if (got_addr == UT64_MAX) {
			got_addr = 0;
		}
	}
	if (got_addr < 1 && bin->ehdr.e_type == ET_REL) {
		got_addr = Elf_(rz_bin_elf_get_section_addr)(bin, ".got.rz");
		if (got_addr == -1) {
			got_addr = 0;
		}
	}
	if (bf->o) {
		if (!(relocs = Elf_(rz_bin_elf_get_relocs)(bin))) {
			return ret;
		}
		size_t i;
		for (i = 0; !relocs[i].last; i++) {
			if (!(ptr = reloc_convert(bin, &relocs[i], got_addr))) {
				continue;
			}
			rz_list_append(ret, ptr);
		}
	}
	return ret;
}

static void _patch_reloc(ut16 e_machine, RzIOBind *iob, RzBinElfReloc *rel, ut64 S, ut64 B, ut64 L) {
	ut64 val;
	ut64 A = rel->addend, P = rel->rva;
	ut8 buf[8];
	switch (e_machine) {
	case EM_ARM:
		val = S + A;
		rz_write_le32(buf, val);
		iob->write_at(iob->io, rel->rva, buf, 4);
		break;
	case EM_AARCH64:
		val = S + A;
		rz_write_le64(buf, val);
		iob->write_at(iob->io, rel->rva, buf, 8);
		break;
	case EM_PPC64: {
		int low = 0, word = 0;
		switch (rel->type) {
		case RZ_PPC64_REL16_HA:
			word = 2;
			val = (S + A - P + 0x8000) >> 16;
			break;
		case RZ_PPC64_REL16_LO:
			word = 2;
			val = (S + A - P) & 0xffff;
			break;
		case RZ_PPC64_REL14:
			low = 14;
			val = (st64)(S + A - P) >> 2;
			break;
		case RZ_PPC64_REL24:
			low = 24;
			val = (st64)(S + A - P) >> 2;
			break;
		case RZ_PPC64_REL32:
			word = 4;
			val = S + A - P;
			break;
		default:
			break;
		}
		if (low) {
			// TODO big-endian
			switch (low) {
			case 14:
				val &= (1 << 14) - 1;
				iob->read_at(iob->io, rel->rva, buf, 2);
				rz_write_le32(buf, (rz_read_le32(buf) & ~((1 << 16) - (1 << 2))) | val << 2);
				iob->write_at(iob->io, rel->rva, buf, 2);
				break;
			case 24:
				val &= (1 << 24) - 1;
				iob->read_at(iob->io, rel->rva, buf, 4);
				rz_write_le32(buf, (rz_read_le32(buf) & ~((1 << 26) - (1 << 2))) | val << 2);
				iob->write_at(iob->io, rel->rva, buf, 4);
				break;
			}
		} else if (word) {
			// TODO big-endian
			switch (word) {
			case 2:
				rz_write_le16(buf, val);
				iob->write_at(iob->io, rel->rva, buf, 2);
				break;
			case 4:
				rz_write_le32(buf, val);
				iob->write_at(iob->io, rel->rva, buf, 4);
				break;
			}
		}
		break;
	}
	case EM_X86_64: {
		int word = 0;
		switch (rel->type) {
		case RZ_X86_64_8:
			word = 1;
			val = S + A;
			break;
		case RZ_X86_64_16:
			word = 2;
			val = S + A;
			break;
		case RZ_X86_64_32:
		case RZ_X86_64_32S:
			word = 4;
			val = S + A;
			break;
		case RZ_X86_64_64:
			word = 8;
			val = S + A;
			break;
		case RZ_X86_64_GLOB_DAT:
		case RZ_X86_64_JUMP_SLOT:
			word = 4;
			val = S;
			break;
		case RZ_X86_64_PC8:
			word = 1;
			val = S + A - P;
			break;
		case RZ_X86_64_PC16:
			word = 2;
			val = S + A - P;
			break;
		case RZ_X86_64_PC32:
			word = 4;
			val = S + A - P;
			break;
		case RZ_X86_64_PC64:
			word = 8;
			val = S + A - P;
			break;
		case RZ_X86_64_PLT32:
			word = 4;
			val = L + A - P;
			break;
		case RZ_X86_64_RELATIVE:
			word = 8;
			val = B + A;
			break;
		default:
			//eprintf ("relocation %d not handle at this time\n", rel->type);
			break;
		}
		switch (word) {
		case 0:
			break;
		case 1:
			buf[0] = val;
			iob->write_at(iob->io, rel->rva, buf, 1);
			break;
		case 2:
			rz_write_le16(buf, val);
			iob->write_at(iob->io, rel->rva, buf, 2);
			break;
		case 4:
			rz_write_le32(buf, val);
			iob->write_at(iob->io, rel->rva, buf, 4);
			break;
		case 8:
			rz_write_le64(buf, val);
			iob->write_at(iob->io, rel->rva, buf, 8);
			break;
		}
		break;
	}
	}
}

static RzList *patch_relocs(RzBinFile *bf) {
	rz_return_val_if_fail(bf, NULL);
	RzBin *b = bf->rbin;
	RzList *ret = NULL;
	RzBinReloc *ptr = NULL;
	RzIOMap *g = NULL;
	HtUU *relocs_by_sym;
	RzBinElfReloc *relcs = NULL;
	RzBinInfo *info;
	ut64 size, offset = 0;

	RzIO *io = b->iob.io;
	if (!io) {
		return NULL;
	}
	RzBinObject *obj = bf->o;
	if (!obj) {
		return NULL;
	}
	struct Elf_(rz_bin_elf_obj_t) *bin = obj->bin_obj;
	info = obj ? obj->info : NULL;
	size_t cdsz = info ? (info->bits == 64 ? 8 : info->bits == 32 ? 4
					     : info->bits == 16       ? 4
								      : 0)
			   : 0;
	if (bin->ehdr.e_type != ET_REL && bin->ehdr.e_type != ET_DYN) {
		return NULL;
	}
	size = bin->g_reloc_num * cdsz;
	if (size == 0) {
		return NULL;
	}

	void **it;
	RzPVector *maps = rz_io_maps(io);
	rz_pvector_foreach (maps, it) {
		RzIOMap *map = *it;
		if (map->itv.addr > offset) {
			offset = map->itv.addr;
			g = map;
		}
	}
	if (!g) {
		return NULL;
	}
	if (!io->cached) {
		eprintf("Warning: run rizin with -e io.cache=true to fix relocations in disassembly\n");
		return NULL;
	}
	ut64 n_vaddr = g->itv.addr + g->itv.size;
	// reserve at least that space
	size = bin->g_reloc_num * cdsz;
	char *muri = rz_str_newf("malloc://%" PFMT64u, size);
	RzIOMap *gotrzmap;
	RzIODesc *gotrzdesc = b->iob.open_at(io, muri, RZ_PERM_R, 0664, n_vaddr, &gotrzmap);
	free(muri);
	if (!gotrzdesc || !gotrzmap) {
		return NULL;
	}
	gotrzmap->name = strdup(".got.rz");

	if (!(relcs = Elf_(rz_bin_elf_get_relocs)(bin))) {
		return NULL;
	}
	if (!(ret = rz_list_newf((RzListFree)free))) {
		return NULL;
	}
	HtUUOptions opt = { 0 };
	if (!(relocs_by_sym = ht_uu_new_opt(&opt))) {
		rz_list_free(ret);
		return NULL;
	}
	ut64 vaddr = n_vaddr;
	size_t i;
	for (i = 0; !relcs[i].last; i++) {
		ut64 sym_addr = 0;
		ut64 plt_entry_addr = vaddr;

		if (relcs[i].sym) {
			if (relcs[i].sym < bin->imports_by_ord_size && bin->imports_by_ord[relcs[i].sym]) {
				bool found;
				sym_addr = ht_uu_find(relocs_by_sym, relcs[i].sym, &found);
				if (found) {
					plt_entry_addr = sym_addr;
				}
			} else if (relcs[i].sym < bin->symbols_by_ord_size && bin->symbols_by_ord[relcs[i].sym]) {
				sym_addr = bin->symbols_by_ord[relcs[i].sym]->vaddr;
				plt_entry_addr = sym_addr;
			}
		}
		// TODO relocation types B, L
		_patch_reloc(bin->ehdr.e_machine, &b->iob, &relcs[i], sym_addr ? sym_addr : vaddr, 0, plt_entry_addr);
		if (!(ptr = reloc_convert(bin, &relcs[i], n_vaddr))) {
			continue;
		}

		if (sym_addr) {
			ptr->vaddr = sym_addr;
		} else {
			ptr->vaddr = vaddr;
			ht_uu_insert(relocs_by_sym, relcs[i].sym, vaddr);
			vaddr += cdsz;
		}
		rz_list_append(ret, ptr);
	}
	ht_uu_free(relocs_by_sym);
	return ret;
}

static void lookup_symbols(RzBinFile *bf, RzBinInfo *ret) {
	RzList *symbols_list = symbols(bf);
	RzListIter *iter;
	RzBinSymbol *symbol;
	bool is_rust = false;
	if (symbols_list) {
		rz_list_foreach (symbols_list, iter, symbol) {
			if (ret->has_canary && is_rust) {
				break;
			}
			if (!strcmp(symbol->name, "_NSConcreteGlobalBlock")) {
				ret->lang = (ret->lang && !strcmp(ret->lang, "c++")) ? "c++ blocks ext." : "c blocks ext.";
			}
			if (!ret->has_canary) {
				if (strstr(symbol->name, "__stack_chk_fail") || strstr(symbol->name, "__stack_smash_handler")) {
					ret->has_canary = true;
				}
			}
			if (!is_rust && !strcmp(symbol->name, "__rust_oom")) {
				is_rust = true;
				ret->lang = "rust";
			}
		}
		rz_list_free(symbols_list);
	}
}

static void lookup_sections(RzBinFile *bf, RzBinInfo *ret) {
	RzList *sections_list = sections(bf);
	RzListIter *iter;
	RzBinSection *section;
	bool is_go = false;
	ret->has_retguard = -1;
	rz_list_foreach (sections_list, iter, section) {
		if (ret->has_retguard != -1 && is_go == true) {
			break;
		}
		if (strstr(section->name, "note.go.buildid") ||
			strstr(section->name, ".gopclntab") ||
			strstr(section->name, ".go_export")) {
			ret->lang = "go";
			is_go = true;
		}
#define RZ_BIN_RANDOMDATA_RETGUARD_SZ 48
		if (!strcmp(section->name, ".openbsd.randomdata")) {
			// The retguard cookie adds 8 per return function inst.
			ret->has_retguard = (section->size >= RZ_BIN_RANDOMDATA_RETGUARD_SZ);
			break;
		}
	}
	rz_list_free(sections_list);
}

static bool has_sanitizers(RzBinFile *bf) {
	bool ret = false;
	RzList *imports_list = imports(bf);
	RzListIter *iter;
	RzBinImport *import;
	rz_list_foreach (imports_list, iter, import) {
		if (strstr(import->name, "__sanitizer") ||
			strstr(import->name, "__ubsan")) {
			ret = true;
			break;
		}
	}
	rz_list_free(imports_list);
	return ret;
}

static RzBinInfo *info(RzBinFile *bf) {
	RzBinInfo *ret = NULL;
	char *str;

	if (!(ret = RZ_NEW0(RzBinInfo))) {
		return NULL;
	}
	ret->file = bf->file
		? strdup(bf->file)
		: NULL;
	void *obj = bf->o->bin_obj;
	if ((str = Elf_(rz_bin_elf_get_rpath)(obj))) {
		ret->rpath = strdup(str);
		free(str);
	} else {
		ret->rpath = strdup("NONE");
	}
	if (!(str = Elf_(rz_bin_elf_get_file_type)(obj))) {
		free(ret);
		return NULL;
	}
	ret->type = str;
	ret->has_pi = (strstr(str, "DYN")) ? 1 : 0;
	ret->has_sanitizers = has_sanitizers(bf);
	if (!(str = Elf_(rz_bin_elf_get_elf_class)(obj))) {
		free(ret);
		return NULL;
	}
	ret->bclass = str;
	if (!(str = Elf_(rz_bin_elf_get_osabi_name)(obj))) {
		free(ret);
		return NULL;
	}
	ret->os = str;
	if (!(str = Elf_(rz_bin_elf_get_osabi_name)(obj))) {
		free(ret);
		return NULL;
	}
	ret->subsystem = str;
	if (!(str = Elf_(rz_bin_elf_get_machine_name)(obj))) {
		free(ret);
		return NULL;
	}
	ret->machine = str;
	if (!(str = Elf_(rz_bin_elf_get_head_flag)(obj))) {
		free(ret);
		return NULL;
	}
	ret->head_flag = str;
	if (!(str = Elf_(rz_bin_elf_get_arch)(obj))) {
		free(ret);
		return NULL;
	}
	ret->arch = str;

	if ((str = Elf_(rz_bin_elf_get_cpu)(obj))) {
		ret->cpu = str;
	}
	if ((str = Elf_(rz_bin_elf_get_abi)(obj))) {
		ret->features = str;
	}

	ret->rclass = strdup("elf");
	ret->bits = Elf_(rz_bin_elf_get_bits)(obj);
	if (!strcmp(ret->arch, "avr")) {
		ret->bits = 16;
	}
	ret->big_endian = Elf_(rz_bin_elf_is_big_endian)(obj);
	ret->has_va = Elf_(rz_bin_elf_has_va)(obj);
	ret->has_nx = Elf_(rz_bin_elf_has_nx)(obj);
	ret->intrp = Elf_(rz_bin_elf_intrp)(obj);
	ret->compiler = Elf_(rz_bin_elf_compiler)(obj);
	ret->dbg_info = 0;
	if (!Elf_(rz_bin_elf_get_stripped)(obj)) {
		ret->dbg_info |= RZ_BIN_DBG_LINENUMS | RZ_BIN_DBG_SYMS | RZ_BIN_DBG_RELOCS;
	} else {
		ret->dbg_info |= RZ_BIN_DBG_STRIPPED;
	}
	if (Elf_(rz_bin_elf_is_static)(obj)) {
		ret->dbg_info |= RZ_BIN_DBG_STATIC;
	}
	lookup_sections(bf, ret);
	lookup_symbols(bf, ret);
	return ret;
}

static RzList *fields(RzBinFile *bf) {
	RzList *ret = rz_list_newf((RzListFree)rz_bin_field_free);
	if (!ret) {
		return NULL;
	}

#define ROW(nam, siz, val, fmt) \
	rz_list_append(ret, rz_bin_field_new(addr, addr, siz, nam, sdb_fmt("0x%08" PFMT64x, (ut64)val), fmt, false));
	if (rz_buf_size(bf->buf) < sizeof(Elf_(Ehdr))) {
		return ret;
	}
	ut64 addr = 0;
	ROW("ELF", 4, rz_buf_read_le32_at(bf->buf, addr), "x");
	addr += 0x10;
	ROW("Type", 2, rz_buf_read_le16_at(bf->buf, addr), "x");
	addr += 0x2;
	ROW("Machine", 2, rz_buf_read_le16_at(bf->buf, addr), "x");
	addr += 0x2;
	ROW("Version", 4, rz_buf_read_le32_at(bf->buf, addr), "x");
	addr += 0x4;

	if (rz_buf_read8_at(bf->buf, 0x04) == 1) {
		ROW("Entry point", 4, rz_buf_read_le32_at(bf->buf, addr), "x");
		addr += 0x4;
		ROW("PhOff", 4, rz_buf_read_le32_at(bf->buf, addr), "x");
		addr += 0x4;
		ROW("ShOff", 4, rz_buf_read_le32_at(bf->buf, addr), "x");
		addr += 0x4;
	} else {
		ROW("Entry point", 8, rz_buf_read_le64_at(bf->buf, addr), "x");
		addr += 0x8;
		ROW("PhOff", 8, rz_buf_read_le64_at(bf->buf, addr), "x");
		addr += 0x8;
		ROW("ShOff", 8, rz_buf_read_le64_at(bf->buf, addr), "x");
		addr += 0x8;
	}

	ROW("Flags", 4, rz_buf_read_le32_at(bf->buf, addr), "x");
	addr += 0x4;
	ROW("EhSize", 2, rz_buf_read_le16_at(bf->buf, addr), "x");
	addr += 0x2;
	ROW("PhentSize", 2, rz_buf_read_le16_at(bf->buf, addr), "x");
	addr += 0x2;
	ROW("PhNum", 2, rz_buf_read_le16_at(bf->buf, addr), "x");
	addr += 0x2;
	ROW("ShentSize", 2, rz_buf_read_le16_at(bf->buf, addr), "x");
	addr += 0x2;
	ROW("ShNum", 2, rz_buf_read_le16_at(bf->buf, addr), "x");
	addr += 0x2;
	ROW("ShrStrndx", 2, rz_buf_read_le16_at(bf->buf, addr), "x");

	return ret;
}

static ut64 size(RzBinFile *bf) {
	ut64 off = 0;
	ut64 len = 0;
	if (!bf->o->sections) {
		RzListIter *iter;
		RzBinSection *section;
		bf->o->sections = sections(bf);
		rz_list_foreach (bf->o->sections, iter, section) {
			if (section->paddr > off) {
				off = section->paddr;
				len = section->size;
			}
		}
	}
	return off + len;
}
