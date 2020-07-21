// Copyright (c) 2013, Facebook, Inc.
// All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name Facebook nor the names of its contributors may be used to
//     endorse or promote products derived from this software without specific
//     prior written permission.
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "fishhook.h"

#include <dlfcn.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach/vm_region.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <printf.h>

#ifdef __LP64__
typedef struct mach_header_64 mach_header_t;
typedef struct segment_command_64 segment_command_t;
typedef struct section_64 section_t;
typedef struct nlist_64 nlist_t;
#define LC_SEGMENT_ARCH_DEPENDENT LC_SEGMENT_64
#else
typedef struct mach_header mach_header_t;
typedef struct segment_command segment_command_t;
typedef struct section section_t;
typedef struct nlist nlist_t;
#define LC_SEGMENT_ARCH_DEPENDENT LC_SEGMENT
#endif

#ifndef SEG_DATA_CONST
#define SEG_DATA_CONST  "__DATA_CONST"
#endif

struct rebindings_entry {
    struct rebinding *rebindings;
    size_t rebindings_nel;
    struct rebindings_entry *next;
};

static struct rebindings_entry *_rebindings_head;

static int prepend_rebindings(struct rebindings_entry **rebindings_head,
                                struct rebinding rebindings[],
                                size_t nel) {
    struct rebindings_entry *new_entry = (struct rebindings_entry *) malloc(sizeof(struct rebindings_entry));
    if (!new_entry) {
      return -1;
    }
    new_entry->rebindings = (struct rebinding *) malloc(sizeof(struct rebinding) * nel);
    if (!new_entry->rebindings) {
      free(new_entry);
      return -1;
    }
    memcpy(new_entry->rebindings, rebindings, sizeof(struct rebinding) * nel);
    new_entry->rebindings_nel = nel;
    new_entry->next = *rebindings_head;
    // 新添加的 new_entry 将成为这个链表的头( _rebindings_head )
    // 之前添加的依次挂在头后面
    *rebindings_head = new_entry;
    return 0;
}

static vm_prot_t get_protection(void *sectionStart) {
    mach_port_t task = mach_task_self();
    vm_size_t size = 0;
    vm_address_t address = (vm_address_t)sectionStart;
    memory_object_name_t object;
#if __LP64__
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    vm_region_basic_info_data_64_t info;
    kern_return_t info_ret = vm_region_64(
        task, &address, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_64_t)&info, &count, &object);
#else
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT;
    vm_region_basic_info_data_t info;
    kern_return_t info_ret = vm_region(task, &address, &size, VM_REGION_BASIC_INFO, (vm_region_info_t)&info, \
                                       &count, &object);
#endif
    if (info_ret == KERN_SUCCESS) {
      return info.protection;
    } else {
      return VM_PROT_READ;
    }
}
static bool isMainImage = false;

static void perform_rebinding_with_section(struct rebindings_entry *rebindings,
                                             section_t *section,
                                             intptr_t slide,
                                             nlist_t *symtab,
                                             char *strtab,
                                             uint32_t *indirect_symtab) {
    const bool isDataConst = strcmp(section->segname, SEG_DATA_CONST) == 0;
    /* __DATA 和 __DATA_CONST 的延迟和非延迟绑定 section 的 reserved1 字段
    section.name = __got, reserved1=31; section.name = __la_symbol_ptr, reserved1 = 34
     通过在间接跳转表中偏移 section->reserved1，找到当前 section （可能为__got 或 __la_symbol_ptr）第一个符号表
            在间接跳转表中的位置
     */
    uint32_t *indirect_symbol_indices = indirect_symtab + section->reserved1;
    if (isMainImage) {// FISHIHOOkDEMO
        printf("Got U!\n");
    }
    /** 1. 二级指针，指向当前 section 的头部
    2. 当前 Section 是 __DATA 类型 Section，懒加载（或非懒加载）符号表里面的每一项的值是一个地址，
        虽然初始值可能不相同，但最终的预期都是指向各自的真实函数的地址
    3. 对于我们来说，需要修改的是指针指向的指针的值，所以此处直接申明为二级指针
     */
    void **indirect_symbol_bindings = (void **)((uintptr_t)slide + section->addr);
    vm_prot_t oldProtection = VM_PROT_READ;
    if (isDataConst) {
        // 对于 __DATA_CONST，需要修改内存权限，暂时修改为可读可写，以便接下来能够将 replacement 地址写入
      oldProtection = get_protection(rebindings);
      mprotect(indirect_symbol_bindings, section->size, PROT_READ | PROT_WRITE);
    }
    // 名字为 __got 或 __la_symbol_ptr 的 Section 就是存放一系列函数地址的列表，所以将 sizeof(void *) 作为循环步进长度
    for (uint i = 0; i < section->size / sizeof(void *); i++) {
        /* 相当于是取 indirect_symbol_indices 指向地址偏移 (sizeof(uint32_t *) * i) 处的值，
                    这是一个索引，可以在 symbol table 中找到具体的 symbol 结构（nlist_64）
         */
      uint32_t symtab_index = indirect_symbol_indices[i];
      if (symtab_index == INDIRECT_SYMBOL_ABS || symtab_index == INDIRECT_SYMBOL_LOCAL ||
          symtab_index == (INDIRECT_SYMBOL_LOCAL   | INDIRECT_SYMBOL_ABS)) {
        continue;
      }
        // 在 symbol table （nlist_64） 中得到当前符号在字符串表中的偏移，并在字符串表中取到当前符号的名字（字符串）
      uint32_t strtab_offset = symtab[symtab_index].n_un.n_strx;
      char *symbol_name = strtab + strtab_offset;
        /* 字符串长度为 0 或者为 1 时，第 0 或第 1 个位置肯定是 '\0'，任意值与它 && 运算时一定为 false。
            为 false 时即字符串说明长度小于等于 1
         */
      bool symbol_name_longer_than_1 = symbol_name[0] && symbol_name[1];
      struct rebindings_entry *cur = rebindings;
      while (cur) { // 典型的链表遍历
        for (uint j = 0; j < cur->rebindings_nel; j++) {
            /* 比较当前遍历的字符串是否和待替换的相同
             `[]`的优先级高于`&`，所以 `&symbol_name[1]` 取的是去掉第一个字符后的字符串（字符串总是以'\0'为终止条件），
                        即去掉编译器在函数前面添加的"_"
             这也是为什么要求字符串长度大于 1 的原因
             */
          if (symbol_name_longer_than_1 &&
              strcmp(&symbol_name[1], cur->rebindings[j].name) == 0) {
            if (cur->rebindings[j].replaced != NULL &&
                indirect_symbol_bindings[i] != cur->rebindings[j].replacement) {
                /* indirect_symbol_bindings[i] 存放的是系统原来的实现函数的指针：
                      对于懒加载符号，如果在这之前已经被绑定过，indirect_symbol_bindings[i] 就是该符号真实地址
                      如果没有被绑定过，indirect_symbol_bindings[i] 就指向 __TEXT.__stub_helper 中
                 不管怎么样，系统原来的实现地址（也可能是间接地址），会被存入 replaced 指针指向的空间中
                 replaced 是一个二级指针，值为二级指针的地址，初始时指向的内容为 0x00
                 二级指针的目的：为了和 indirect_symbol_bindings 的结构保持一致，
                        以便在首次调用时能够借助 __TEXT.__stub_helper 将符号的真实地址写回来
                 */
              *(cur->rebindings[j].replaced) = indirect_symbol_bindings[i];
            }
              /* 新的函数地址写入到了当前 Section 的符号表项目中，以后在调用时读取符号表的此项目 Data 时，
                        读取的就是 replacement 的地址
               */
            indirect_symbol_bindings[i] = cur->rebindings[j].replacement;
              // 如果找到了，对符号表 Section 中的下一个符号进行检查
            goto symbol_loop;
          }
        }
        cur = cur->next;
      }
    symbol_loop:;
    }
    if (isDataConst) {
        // __DATA_CONST 类型的 Section 内存写入完成后，恢复成原来的内存权限
      int protection = 0;
      if (oldProtection & VM_PROT_READ) {
        protection |= PROT_READ;
      }
      if (oldProtection & VM_PROT_WRITE) {
        protection |= PROT_WRITE;
      }
      if (oldProtection & VM_PROT_EXECUTE) {
        protection |= PROT_EXEC;
      }
      mprotect(indirect_symbol_bindings, section->size, protection);
    }
}

static void rebind_symbols_for_image(struct rebindings_entry *rebindings,
                                       const struct mach_header *header,
                                       intptr_t slide) {
    Dl_info info;
    
    /**
       向 dyld 查询 header 指向的地址是否存在于某一个 image 里面。
       如果存在，返回非 0；info 中存放具体信息
       如果返回为 0 表示不存在
    */
    if (dladdr(header, &info) == 0) {
      return;
    }
    
    // 如果 pathname 以 /private/var 开头，可以认为就是 execute 这个 image
    const char *pathname = info.dli_fname;
    
    if (strlen(pathname) > 8) {
        char subPathname[9] = {'\0'};
        char target[9] = "/private";
        strncpy(subPathname, pathname, 8);
        if (!strcmp(subPathname, target)) {
            isMainImage = true;
        }
    }

    segment_command_t *cur_seg_cmd;
    segment_command_t *linkedit_segment = NULL;
    struct symtab_command* symtab_cmd = NULL;
    struct dysymtab_command* dysymtab_cmd = NULL;
    /* 通过遍历找到 Load Commnads 中 LC_SEGMENT_64.info（）、LC_SYMTAB(规定
                了 symbol table 和 string table 在文件中的位置与大小)、LC_DYSYMTAB，
                    并用指针分别指向他们
     */
    uintptr_t cur = (uintptr_t)header + sizeof(mach_header_t);
    for (uint i = 0; i < header->ncmds; i++, cur += cur_seg_cmd->cmdsize) {
      cur_seg_cmd = (segment_command_t *)cur;
      if (cur_seg_cmd->cmd == LC_SEGMENT_ARCH_DEPENDENT) {
        if (strcmp(cur_seg_cmd->segname, SEG_LINKEDIT) == 0) {
          linkedit_segment = cur_seg_cmd;
        }
      } else if (cur_seg_cmd->cmd == LC_SYMTAB) {
        symtab_cmd = (struct symtab_command*)cur_seg_cmd;
      } else if (cur_seg_cmd->cmd == LC_DYSYMTAB) {
        dysymtab_cmd = (struct dysymtab_command*)cur_seg_cmd;
      }
    }

    if (!symtab_cmd || !dysymtab_cmd || !linkedit_segment ||
        !dysymtab_cmd->nindirectsyms) {
      return;
    }

    /* Find base symbol/string table addresses
     ASLR 地址 + linkedit 描述的 segment 被加载到内存后的地址 - linkedit 在文件中的偏移 =
                        文件开始位置在内存中的地址（file Offset 为 0 的地方）的值
     如果 16 进制查看（ p/x linkedit_base）其值，会发现和 header 指针相同
    */
    uintptr_t linkedit_base = (uintptr_t)slide + linkedit_segment->vmaddr - linkedit_segment->fileoff;
    // 得到 LC_SYMTAB 在内存中的地址。体现了 Mach-O 的某一处相对于文件偏移的位置，是如何对应到运行时的内存地址
    // symbol table 是一个 nlist_64 结构体数组，所以指向结构体数组指针
    nlist_t *symtab = (nlist_t *)(linkedit_base + symtab_cmd->symoff);
    //  strtab 以 '\0' 分割的，列出了所有可见字符串
    // string table 就是 ASCII 组成的数组，所以使用 char* 描述
    char *strtab = (char *)(linkedit_base + symtab_cmd->stroff);
    
    
    // Get indirect symbol table (array of uint32_t indices into symbol table)
    // Dynamic Symbol Table (Indirect Symbols) 在内存中的地址
    uint32_t *indirect_symtab = (uint32_t *)(linkedit_base + dysymtab_cmd->indirectsymoff);

    cur = (uintptr_t)header + sizeof(mach_header_t);
    // 每遍历一个，序号 i ++，cur 指向下一个 load command 的头部
    for (uint i = 0; i < header->ncmds; i++, cur += cur_seg_cmd->cmdsize) {
      cur_seg_cmd = (segment_command_t *)cur;
      if (cur_seg_cmd->cmd == LC_SEGMENT_ARCH_DEPENDENT) {
        if (strcmp(cur_seg_cmd->segname, SEG_DATA) != 0 &&
            strcmp(cur_seg_cmd->segname, SEG_DATA_CONST) != 0) {
          continue;
        }
          /* segment.name 为 __DATA 或 __DATA_CONST，因为动态链接库的符号的 stub 就在里面指定。
           __TEXT.__stubs 的这里不用管，因为当某一个延迟绑定的函数被调用时，
           一定会被转 __DATA_CONST和__DATA的 __got 或 __la_symbol_ptr区域
           
           非延迟绑定的符号(来自于 __DATA_CONST.__got) 也能被重新绑定，
           因为它和延迟绑定的符号工作机制相同，只是绑定时机不同
           */
        for (uint j = 0; j < cur_seg_cmd->nsects; j++) {
          section_t *sect =
            (section_t *)(cur + sizeof(segment_command_t)) + j;
            // 遍历 __DATA 和 __DATA_CONST 的每一个 section，找到延迟绑定和非延迟绑定符号所在的 section，然后琢磨着偷梁换柱
          if ((sect->flags & SECTION_TYPE) == S_LAZY_SYMBOL_POINTERS) {
              if (isMainImage) {
                  printf("section.name=%s,reserved1=%d\n\n",sect->sectname,sect->reserved1);
              }
            perform_rebinding_with_section(rebindings, sect, slide, symtab, strtab, indirect_symtab);
          }
          if ((sect->flags & SECTION_TYPE) == S_NON_LAZY_SYMBOL_POINTERS) {
              if (isMainImage) {
                  printf("section.name=%s,reserved1=%d\n\n",sect->sectname,sect->reserved1);
              }
            perform_rebinding_with_section(rebindings, sect, slide, symtab, strtab, indirect_symtab);
          }
        }
      }
    }
}

static void _rebind_symbols_for_image(const struct mach_header *header,
                                        intptr_t slide) {
      rebind_symbols_for_image(_rebindings_head, header, slide);
}

int rebind_symbols_image(void *header,
                           intptr_t slide,
                           struct rebinding rebindings[],
                           size_t rebindings_nel) {
      struct rebindings_entry *rebindings_head = NULL;
      int retval = prepend_rebindings(&rebindings_head, rebindings, rebindings_nel);
      rebind_symbols_for_image(rebindings_head, (const struct mach_header *) header, slide);
      if (rebindings_head) {
        free(rebindings_head->rebindings);
      }
      free(rebindings_head);
      return retval;
}

int rebind_symbols(struct rebinding rebindings[], size_t rebindings_nel) {
    int retval = prepend_rebindings(&_rebindings_head, rebindings, rebindings_nel);
    if (retval < 0) {
      return retval;
    }

    if (!_rebindings_head->next) {
        /**
            _dyld_register_func_for_add_image 被调用时，
                已经被 dyld 加载的 image 会立刻回调，
                后续被 dyld 新加载的 image 也会触发回调
         */
      _dyld_register_func_for_add_image(_rebind_symbols_for_image);
    } else {
      uint32_t c = _dyld_image_count();
      for (uint32_t i = 0; i < c; i++) {
        _rebind_symbols_for_image(_dyld_get_image_header(i), _dyld_get_image_vmaddr_slide(i));
      }
    }
    return retval;
}
