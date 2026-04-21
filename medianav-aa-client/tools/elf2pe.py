#!/usr/bin/env python3
"""
elf2pe.py - Convert MIPS ELF to Windows CE PE Executable

Reads a statically-linked MIPS ELF binary and produces a PE (Portable Executable)
file that can theoretically run on Windows CE 6.0 MIPS.

IMPORTANT LIMITATIONS:
1. This produces a MINIMAL PE with .text and .data sections only
2. No import table (no coredll.dll references) - the binary is fully static
3. No WinCE-specific subsystem metadata
4. This is a PROOF OF CONCEPT - the resulting .exe may need manual fixup
   for actual WinCE execution (entry point, import tables, etc.)

For a production WinCE binary, use VS2005 with Platform Builder.
This tool is for demonstrating that the MIPS machine code is valid.

Usage: python3 elf2pe.py input.elf output.exe
"""

import struct
import sys
import os

# PE Constants
IMAGE_FILE_MACHINE_R4000 = 0x0166  # MIPS little-endian (R4000)
IMAGE_FILE_EXECUTABLE_IMAGE = 0x0002
IMAGE_FILE_32BIT_MACHINE = 0x0100
IMAGE_FILE_DLL = 0x2000

PE_SIGNATURE = b'PE\x00\x00'
DOS_HEADER_SIZE = 128  # Minimal DOS header
PE_HEADER_SIZE = 24     # COFF header
OPTIONAL_HEADER_SIZE = 224  # PE32 optional header
SECTION_HEADER_SIZE = 40

# WinCE subsystem
IMAGE_SUBSYSTEM_WINDOWS_CE_GUI = 9

# Section characteristics
IMAGE_SCN_CNT_CODE = 0x00000020
IMAGE_SCN_CNT_INITIALIZED_DATA = 0x00000040
IMAGE_SCN_MEM_EXECUTE = 0x20000000
IMAGE_SCN_MEM_READ = 0x40000000
IMAGE_SCN_MEM_WRITE = 0x80000000

FILE_ALIGNMENT = 0x200  # 512 bytes
SECTION_ALIGNMENT = 0x1000  # 4KB

def align(val, alignment):
    return (val + alignment - 1) & ~(alignment - 1)

def read_elf(filename):
    """Read ELF file and extract sections."""
    with open(filename, 'rb') as f:
        data = f.read()

    # Verify ELF magic
    if data[:4] != b'\x7fELF':
        raise ValueError("Not an ELF file")

    # ELF32 Little Endian
    ei_class = data[4]
    ei_data = data[5]
    if ei_class != 1:  # ELFCLASS32
        raise ValueError("Not ELF32")
    if ei_data != 1:  # ELFDATA2LSB
        raise ValueError("Not little-endian")

    # Parse ELF header
    e_type, e_machine = struct.unpack_from('<HH', data, 16)
    e_entry = struct.unpack_from('<I', data, 24)[0]
    e_phoff = struct.unpack_from('<I', data, 28)[0]
    e_shoff = struct.unpack_from('<I', data, 32)[0]
    e_ehsize = struct.unpack_from('<H', data, 40)[0]
    e_phentsize = struct.unpack_from('<H', data, 42)[0]
    e_phnum = struct.unpack_from('<H', data, 44)[0]
    e_shentsize = struct.unpack_from('<H', data, 46)[0]
    e_shnum = struct.unpack_from('<H', data, 48)[0]
    e_shstrndx = struct.unpack_from('<H', data, 50)[0]

    print(f"ELF: machine=0x{e_machine:04X}, entry=0x{e_entry:08X}")
    print(f"     {e_phnum} program headers, {e_shnum} section headers")

    # Read section headers
    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh = struct.unpack_from('<IIIIIIIIII', data, off)
        sections.append({
            'name_idx': sh[0],
            'type': sh[1],
            'flags': sh[2],
            'addr': sh[3],
            'offset': sh[4],
            'size': sh[5],
            'link': sh[6],
            'info': sh[7],
            'addralign': sh[8],
            'entsize': sh[9],
        })

    # Read section name string table
    if e_shstrndx < len(sections):
        strtab = sections[e_shstrndx]
        strtab_data = data[strtab['offset']:strtab['offset']+strtab['size']]
    else:
        strtab_data = b''

    for s in sections:
        name_end = strtab_data.find(b'\x00', s['name_idx'])
        if name_end >= 0:
            s['name'] = strtab_data[s['name_idx']:name_end].decode('ascii', errors='replace')
        else:
            s['name'] = ''

    # Read program headers (LOAD segments)
    segments = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        ph = struct.unpack_from('<IIIIIIII', data, off)
        if ph[0] == 1:  # PT_LOAD
            segments.append({
                'type': ph[0],
                'offset': ph[1],
                'vaddr': ph[2],
                'paddr': ph[3],
                'filesz': ph[4],
                'memsz': ph[5],
                'flags': ph[6],
                'align': ph[7],
            })

    # Extract code and data
    code_data = b''
    data_data = b''
    code_addr = 0
    data_addr = 0

    for seg in segments:
        seg_data = data[seg['offset']:seg['offset']+seg['filesz']]
        if seg['flags'] & 1:  # PF_X (executable)
            if not code_data or seg['vaddr'] < code_addr:
                code_addr = seg['vaddr']
            code_data += seg_data
        else:
            if not data_data or seg['vaddr'] < data_addr:
                data_addr = seg['vaddr']
            data_data += seg_data

    # If code and data overlap (common in static binaries), treat it all as one
    if not code_data and segments:
        # All segments as one code section
        min_addr = min(s['vaddr'] for s in segments)
        max_end = max(s['vaddr'] + s['filesz'] for s in segments)
        code_addr = min_addr
        combined = bytearray(max_end - min_addr)
        for seg in segments:
            offset = seg['vaddr'] - min_addr
            seg_bytes = data[seg['offset']:seg['offset']+seg['filesz']]
            combined[offset:offset+len(seg_bytes)] = seg_bytes
        code_data = bytes(combined)
        data_data = b''

    return {
        'entry': e_entry,
        'code': code_data,
        'code_addr': code_addr,
        'data': data_data,
        'data_addr': data_addr,
        'sections': sections,
        'segments': segments,
    }


def build_pe(elf_info, output_filename):
    """Build a PE executable from ELF data."""

    code = elf_info['code']
    data = elf_info['data'] if elf_info['data'] else b''
    entry_rva = elf_info['entry'] - elf_info['code_addr'] + SECTION_ALIGNMENT

    num_sections = 1 if not data else 2

    # Calculate sizes
    headers_size = DOS_HEADER_SIZE + len(PE_SIGNATURE) + PE_HEADER_SIZE + \
                   OPTIONAL_HEADER_SIZE + (num_sections * SECTION_HEADER_SIZE)
    headers_size_aligned = align(headers_size, FILE_ALIGNMENT)

    code_file_size = align(len(code), FILE_ALIGNMENT)
    data_file_size = align(len(data), FILE_ALIGNMENT) if data else 0

    code_file_offset = headers_size_aligned
    data_file_offset = code_file_offset + code_file_size if data else 0

    code_rva = SECTION_ALIGNMENT
    data_rva = align(code_rva + len(code), SECTION_ALIGNMENT) if data else 0

    image_size = align(
        (data_rva + len(data)) if data else (code_rva + len(code)),
        SECTION_ALIGNMENT
    )

    total_file_size = headers_size_aligned + code_file_size + data_file_size

    print(f"PE: entry_rva=0x{entry_rva:08X}")
    print(f"    code: {len(code)} bytes at file offset 0x{code_file_offset:X}, RVA 0x{code_rva:X}")
    if data:
        print(f"    data: {len(data)} bytes at file offset 0x{data_file_offset:X}, RVA 0x{data_rva:X}")
    print(f"    image size: {image_size} bytes")
    print(f"    file size: {total_file_size} bytes")

    pe = bytearray(total_file_size)

    # ---- DOS Header (minimal stub) ----
    pe[0:2] = b'MZ'
    # e_lfanew: offset to PE signature
    struct.pack_into('<I', pe, 60, DOS_HEADER_SIZE)

    # ---- PE Signature ----
    offset = DOS_HEADER_SIZE
    pe[offset:offset+4] = PE_SIGNATURE
    offset += 4

    # ---- COFF Header (20 bytes) ----
    coff_flags = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_32BIT_MACHINE
    struct.pack_into('<HH', pe, offset, IMAGE_FILE_MACHINE_R4000, num_sections)
    offset += 4
    struct.pack_into('<I', pe, offset, 0)  # TimeDateStamp
    offset += 4
    struct.pack_into('<I', pe, offset, 0)  # PointerToSymbolTable
    offset += 4
    struct.pack_into('<I', pe, offset, 0)  # NumberOfSymbols
    offset += 4
    struct.pack_into('<HH', pe, offset, OPTIONAL_HEADER_SIZE, coff_flags)
    offset += 4

    # ---- Optional Header (PE32, 224 bytes) ----
    opt_start = offset
    struct.pack_into('<H', pe, offset, 0x010B)  # PE32 magic
    offset += 2
    pe[offset] = 1; pe[offset+1] = 0  # Linker version
    offset += 2
    struct.pack_into('<I', pe, offset, len(code))  # SizeOfCode
    offset += 4
    struct.pack_into('<I', pe, offset, len(data))  # SizeOfInitializedData
    offset += 4
    struct.pack_into('<I', pe, offset, 0)  # SizeOfUninitializedData
    offset += 4
    struct.pack_into('<I', pe, offset, entry_rva)  # AddressOfEntryPoint
    offset += 4
    struct.pack_into('<I', pe, offset, code_rva)  # BaseOfCode
    offset += 4
    struct.pack_into('<I', pe, offset, data_rva if data else 0)  # BaseOfData
    offset += 4

    # NT-specific fields
    struct.pack_into('<I', pe, offset, 0x00010000)  # ImageBase
    offset += 4
    struct.pack_into('<I', pe, offset, SECTION_ALIGNMENT)  # SectionAlignment
    offset += 4
    struct.pack_into('<I', pe, offset, FILE_ALIGNMENT)  # FileAlignment
    offset += 4

    # OS version (CE 6.0)
    struct.pack_into('<HH', pe, offset, 6, 0)
    offset += 4
    # Image version
    struct.pack_into('<HH', pe, offset, 0, 0)
    offset += 4
    # Subsystem version (CE 6.0)
    struct.pack_into('<HH', pe, offset, 6, 0)
    offset += 4
    # Win32VersionValue
    struct.pack_into('<I', pe, offset, 0)
    offset += 4
    # SizeOfImage
    struct.pack_into('<I', pe, offset, image_size)
    offset += 4
    # SizeOfHeaders
    struct.pack_into('<I', pe, offset, headers_size_aligned)
    offset += 4
    # CheckSum
    struct.pack_into('<I', pe, offset, 0)
    offset += 4
    # Subsystem: Windows CE GUI
    struct.pack_into('<H', pe, offset, IMAGE_SUBSYSTEM_WINDOWS_CE_GUI)
    offset += 2
    # DllCharacteristics
    struct.pack_into('<H', pe, offset, 0)
    offset += 2
    # Stack/Heap sizes
    struct.pack_into('<IIII', pe, offset,
                     0x100000,  # SizeOfStackReserve (1MB)
                     0x10000,   # SizeOfStackCommit (64KB)
                     0x100000,  # SizeOfHeapReserve
                     0x1000)    # SizeOfHeapCommit
    offset += 16
    # LoaderFlags
    struct.pack_into('<I', pe, offset, 0)
    offset += 4
    # NumberOfRvaAndSizes (16 data directories)
    struct.pack_into('<I', pe, offset, 16)
    offset += 4
    # Data directories (all zeros - no imports/exports)
    offset += 16 * 8

    # ---- Section Headers ----
    # .text section
    name = b'.text\x00\x00\x00'
    pe[offset:offset+8] = name
    offset += 8
    struct.pack_into('<II', pe, offset, len(code), code_rva)  # VirtualSize, VirtualAddress
    offset += 8
    struct.pack_into('<II', pe, offset, code_file_size, code_file_offset)  # RawSize, RawPtr
    offset += 8
    struct.pack_into('<III', pe, offset, 0, 0, 0)  # Relocations, LineNumbers, Relocs/Lines count
    offset += 12
    struct.pack_into('<I', pe, offset,
                     IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ)
    offset += 4

    if data:
        # .data section
        name = b'.data\x00\x00\x00'
        pe[offset:offset+8] = name
        offset += 8
        struct.pack_into('<II', pe, offset, len(data), data_rva)
        offset += 8
        struct.pack_into('<II', pe, offset, data_file_size, data_file_offset)
        offset += 8
        struct.pack_into('<III', pe, offset, 0, 0, 0)
        offset += 12
        struct.pack_into('<I', pe, offset,
                         IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE)
        offset += 4

    # ---- Write section data ----
    pe[code_file_offset:code_file_offset+len(code)] = code
    if data:
        pe[data_file_offset:data_file_offset+len(data)] = data

    with open(output_filename, 'wb') as f:
        f.write(pe)

    print(f"\nWrote {len(pe)} bytes to {output_filename}")
    print(f"Machine: MIPS R4000 (0x{IMAGE_FILE_MACHINE_R4000:04X})")
    print(f"Subsystem: Windows CE GUI ({IMAGE_SUBSYSTEM_WINDOWS_CE_GUI})")


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input.elf> <output.exe>")
        sys.exit(1)

    elf_file = sys.argv[1]
    pe_file = sys.argv[2]

    print(f"Converting {elf_file} -> {pe_file}")
    print()

    elf_info = read_elf(elf_file)
    build_pe(elf_info, pe_file)

    print()
    print("WARNING: This PE is experimental. It contains valid MIPS machine code")
    print("but may need import table fixups for actual WinCE execution.")
    print("For production use, compile with VS2005 + Platform Builder SDK.")


if __name__ == '__main__':
    main()
