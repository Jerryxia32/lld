//===- SyntheticSections.cpp ----------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains linker-synthesized sections. Currently,
// synthetic sections are created either output sections or input sections,
// but we are rewriting code so that all synthetic sections are created as
// input sections.
//
//===----------------------------------------------------------------------===//

#include "SyntheticSections.h"
#include "Config.h"
#include "Error.h"
#include "InputFiles.h"
#include "LinkerScript.h"
#include "Memory.h"
#include "OutputSections.h"
#include "Strings.h"
#include "SymbolTable.h"
#include "Target.h"
#include "Threads.h"
#include "Writer.h"
#include "lld/Config/Version.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugPubTable.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/Dwarf.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/RandomNumberGenerator.h"
#include "llvm/Support/SHA1.h"
#include "llvm/Support/xxhash.h"
#include <cstdlib>

using namespace llvm;
using namespace llvm::dwarf;
using namespace llvm::ELF;
using namespace llvm::object;
using namespace llvm::support;
using namespace llvm::support::endian;

using namespace lld;
using namespace lld::elf;

uint64_t SyntheticSection::getVA() const {
  if (this->OutSec)
    return this->OutSec->Addr + this->OutSecOff;
  return 0;
}

template <class ELFT> static std::vector<DefinedCommon *> getCommonSymbols() {
  std::vector<DefinedCommon *> V;
  for (Symbol *S : Symtab<ELFT>::X->getSymbols())
    if (auto *B = dyn_cast<DefinedCommon>(S->body()))
      V.push_back(B);
  return V;
}

// Find all common symbols and allocate space for them.
template <class ELFT> InputSection *elf::createCommonSection() {
  if (!Config->DefineCommon)
    return nullptr;

  // Sort the common symbols by alignment as an heuristic to pack them better.
  std::vector<DefinedCommon *> Syms = getCommonSymbols<ELFT>();
  if (Syms.empty())
    return nullptr;

  std::stable_sort(Syms.begin(), Syms.end(),
                   [](const DefinedCommon *A, const DefinedCommon *B) {
                     return A->Alignment > B->Alignment;
                   });

  BssSection *Sec = make<BssSection>("COMMON");
  for (DefinedCommon *Sym : Syms)
    Sym->Offset = Sec->reserveSpace(Sym->Size, Sym->Alignment);
  return Sec;
}

// Returns an LLD version string.
static ArrayRef<uint8_t> getVersion() {
  // Check LLD_VERSION first for ease of testing.
  // You can get consitent output by using the environment variable.
  // This is only for testing.
  StringRef S = getenv("LLD_VERSION");
  if (S.empty())
    S = Saver.save(Twine("Linker: ") + getLLDVersion());

  // +1 to include the terminating '\0'.
  return {(const uint8_t *)S.data(), S.size() + 1};
}

// Creates a .comment section containing LLD version info.
// With this feature, you can identify LLD-generated binaries easily
// by "readelf --string-dump .comment <file>".
// The returned object is a mergeable string section.
template <class ELFT> MergeInputSection *elf::createCommentSection() {
  typename ELFT::Shdr Hdr = {};
  Hdr.sh_flags = SHF_MERGE | SHF_STRINGS;
  Hdr.sh_type = SHT_PROGBITS;
  Hdr.sh_entsize = 1;
  Hdr.sh_addralign = 1;

  auto *Ret =
      make<MergeInputSection>((ObjectFile<ELFT> *)nullptr, &Hdr, ".comment");
  Ret->Data = getVersion();
  Ret->splitIntoPieces();
  return Ret;
}

// .MIPS.abiflags section.
template <class ELFT>
MipsAbiFlagsSection<ELFT>::MipsAbiFlagsSection(Elf_Mips_ABIFlags Flags)
    : SyntheticSection(SHF_ALLOC, SHT_MIPS_ABIFLAGS, 8, ".MIPS.abiflags"),
      Flags(Flags) {
  this->Entsize = sizeof(Elf_Mips_ABIFlags);
}

template <class ELFT> void MipsAbiFlagsSection<ELFT>::writeTo(uint8_t *Buf) {
  memcpy(Buf, &Flags, sizeof(Flags));
}

template <class ELFT>
MipsAbiFlagsSection<ELFT> *MipsAbiFlagsSection<ELFT>::create() {
  Elf_Mips_ABIFlags Flags = {};
  bool Create = false;

  for (InputSectionBase *Sec : InputSections) {
    if (Sec->Type != SHT_MIPS_ABIFLAGS)
      continue;
    Sec->Live = false;
    Create = true;

    std::string Filename = toString(Sec->getFile<ELFT>());
    const size_t Size = Sec->Data.size();
    // Older version of BFD (such as the default FreeBSD linker) concatenate
    // .MIPS.abiflags instead of merging. To allow for this case (or potential
    // zero padding) we ignore everything after the first Elf_Mips_ABIFlags
    if (Size < sizeof(Elf_Mips_ABIFlags)) {
      error(Filename + ": invalid size of .MIPS.abiflags section: got " +
            Twine(Size) + " instead of " + Twine(sizeof(Elf_Mips_ABIFlags)));
      return nullptr;
    }
    auto *S = reinterpret_cast<const Elf_Mips_ABIFlags *>(Sec->Data.data());
    if (S->version != 0) {
      error(Filename + ": unexpected .MIPS.abiflags version " +
            Twine(S->version));
      return nullptr;
    }
    if (Size > sizeof(Elf_Mips_ABIFlags)) {
      warn(Filename + ": .MIPS.abiflags section has multiple entries: got " +
          Twine(Size) + " instead of " + Twine(sizeof(Elf_Mips_ABIFlags)) +
          " bytes");
    }

    // LLD checks ISA compatibility in getMipsEFlags(). Here we just
    // select the highest number of ISA/Rev/Ext.
    Flags.isa_level = std::max(Flags.isa_level, S->isa_level);
    Flags.isa_rev = std::max(Flags.isa_rev, S->isa_rev);
    Flags.isa_ext = std::max(Flags.isa_ext, S->isa_ext);
    Flags.gpr_size = std::max(Flags.gpr_size, S->gpr_size);
    Flags.cpr1_size = std::max(Flags.cpr1_size, S->cpr1_size);
    Flags.cpr2_size = std::max(Flags.cpr2_size, S->cpr2_size);
    Flags.ases |= S->ases;
    Flags.flags1 |= S->flags1;
    Flags.flags2 |= S->flags2;
    Flags.fp_abi = elf::getMipsFpAbiFlag(Flags.fp_abi, S->fp_abi, Filename);
  };

  if (Create)
    return make<MipsAbiFlagsSection<ELFT>>(Flags);
  return nullptr;
}

// .MIPS.options section.
template <class ELFT>
MipsOptionsSection<ELFT>::MipsOptionsSection(Elf_Mips_RegInfo Reginfo)
    : SyntheticSection(SHF_ALLOC, SHT_MIPS_OPTIONS, 8, ".MIPS.options"),
      Reginfo(Reginfo) {
  this->Entsize = sizeof(Elf_Mips_Options) + sizeof(Elf_Mips_RegInfo);
}

template <class ELFT> void MipsOptionsSection<ELFT>::writeTo(uint8_t *Buf) {
  auto *Options = reinterpret_cast<Elf_Mips_Options *>(Buf);
  Options->kind = ODK_REGINFO;
  Options->size = getSize();

  if (!Config->Relocatable)
    Reginfo.ri_gp_value = InX::MipsGot->getGp();
  memcpy(Buf + sizeof(Elf_Mips_Options), &Reginfo, sizeof(Reginfo));
}

template <class ELFT>
MipsOptionsSection<ELFT> *MipsOptionsSection<ELFT>::create() {
  // N64 ABI only.
  if (!ELFT::Is64Bits)
    return nullptr;

  Elf_Mips_RegInfo Reginfo = {};
  bool Create = false;

  for (InputSectionBase *Sec : InputSections) {
    if (Sec->Type != SHT_MIPS_OPTIONS)
      continue;
    Sec->Live = false;
    Create = true;

    std::string Filename = toString(Sec->getFile<ELFT>());
    ArrayRef<uint8_t> D = Sec->Data;

    while (!D.empty()) {
      if (D.size() < sizeof(Elf_Mips_Options)) {
        error(Filename + ": invalid size of .MIPS.options section");
        break;
      }

      auto *Opt = reinterpret_cast<const Elf_Mips_Options *>(D.data());
      if (Opt->kind == ODK_REGINFO) {
        if (Config->Relocatable && Opt->getRegInfo().ri_gp_value)
          error(Filename + ": unsupported non-zero ri_gp_value");
        Reginfo.ri_gprmask |= Opt->getRegInfo().ri_gprmask;
        Sec->getFile<ELFT>()->MipsGp0 = Opt->getRegInfo().ri_gp_value;
        break;
      }

      if (!Opt->size)
        fatal(Filename + ": zero option descriptor size");
      D = D.slice(Opt->size);
    }
  };

  if (Create)
    return make<MipsOptionsSection<ELFT>>(Reginfo);
  return nullptr;
}

// MIPS .reginfo section.
template <class ELFT>
MipsReginfoSection<ELFT>::MipsReginfoSection(Elf_Mips_RegInfo Reginfo)
    : SyntheticSection(SHF_ALLOC, SHT_MIPS_REGINFO, 4, ".reginfo"),
      Reginfo(Reginfo) {
  this->Entsize = sizeof(Elf_Mips_RegInfo);
}

template <class ELFT> void MipsReginfoSection<ELFT>::writeTo(uint8_t *Buf) {
  if (!Config->Relocatable)
    Reginfo.ri_gp_value = InX::MipsGot->getGp();
  memcpy(Buf, &Reginfo, sizeof(Reginfo));
}

template <class ELFT>
MipsReginfoSection<ELFT> *MipsReginfoSection<ELFT>::create() {
  // Section should be alive for O32 and N32 ABIs only.
  if (ELFT::Is64Bits)
    return nullptr;

  Elf_Mips_RegInfo Reginfo = {};
  bool Create = false;

  for (InputSectionBase *Sec : InputSections) {
    if (Sec->Type != SHT_MIPS_REGINFO)
      continue;
    Sec->Live = false;
    Create = true;

    if (Sec->Data.size() != sizeof(Elf_Mips_RegInfo)) {
      error(toString(Sec->getFile<ELFT>()) +
            ": invalid size of .reginfo section");
      return nullptr;
    }
    auto *R = reinterpret_cast<const Elf_Mips_RegInfo *>(Sec->Data.data());
    if (Config->Relocatable && R->ri_gp_value)
      error(toString(Sec->getFile<ELFT>()) +
            ": unsupported non-zero ri_gp_value");

    Reginfo.ri_gprmask |= R->ri_gprmask;
    Sec->getFile<ELFT>()->MipsGp0 = R->ri_gp_value;
  };

  if (Create)
    return make<MipsReginfoSection<ELFT>>(Reginfo);
  return nullptr;
}

InputSection *elf::createInterpSection() {
  // StringSaver guarantees that the returned string ends with '\0'.
  StringRef S = Saver.save(Config->DynamicLinker);
  ArrayRef<uint8_t> Contents = {(const uint8_t *)S.data(), S.size() + 1};

  auto *Sec =
      make<InputSection>(SHF_ALLOC, SHT_PROGBITS, 1, Contents, ".interp");
  Sec->Live = true;
  return Sec;
}

SymbolBody *elf::addSyntheticLocal(StringRef Name, uint8_t Type, uint64_t Value,
                                   uint64_t Size, InputSectionBase *Section) {
  auto *S = make<DefinedRegular>(Name, /*IsLocal*/ true, STV_DEFAULT, Type,
                                 Value, Size, Section, nullptr);
  if (InX::SymTab)
    InX::SymTab->addSymbol(S);
  return S;
}

static size_t getHashSize() {
  switch (Config->BuildId) {
  case BuildIdKind::Fast:
    return 8;
  case BuildIdKind::Md5:
  case BuildIdKind::Uuid:
    return 16;
  case BuildIdKind::Sha1:
    return 20;
  case BuildIdKind::Hexstring:
    return Config->BuildIdVector.size();
  default:
    llvm_unreachable("unknown BuildIdKind");
  }
}

BuildIdSection::BuildIdSection()
    : SyntheticSection(SHF_ALLOC, SHT_NOTE, 1, ".note.gnu.build-id"),
      HashSize(getHashSize()) {}

void BuildIdSection::writeTo(uint8_t *Buf) {
  endianness E = Config->Endianness;
  write32(Buf, 4, E);                   // Name size
  write32(Buf + 4, HashSize, E);        // Content size
  write32(Buf + 8, NT_GNU_BUILD_ID, E); // Type
  memcpy(Buf + 12, "GNU", 4);           // Name string
  HashBuf = Buf + 16;
}

// Split one uint8 array into small pieces of uint8 arrays.
static std::vector<ArrayRef<uint8_t>> split(ArrayRef<uint8_t> Arr,
                                            size_t ChunkSize) {
  std::vector<ArrayRef<uint8_t>> Ret;
  while (Arr.size() > ChunkSize) {
    Ret.push_back(Arr.take_front(ChunkSize));
    Arr = Arr.drop_front(ChunkSize);
  }
  if (!Arr.empty())
    Ret.push_back(Arr);
  return Ret;
}

// Computes a hash value of Data using a given hash function.
// In order to utilize multiple cores, we first split data into 1MB
// chunks, compute a hash for each chunk, and then compute a hash value
// of the hash values.
void BuildIdSection::computeHash(
    llvm::ArrayRef<uint8_t> Data,
    std::function<void(uint8_t *Dest, ArrayRef<uint8_t> Arr)> HashFn) {
  std::vector<ArrayRef<uint8_t>> Chunks = split(Data, 1024 * 1024);
  std::vector<uint8_t> Hashes(Chunks.size() * HashSize);

  // Compute hash values.
  parallelForEachN(0, Chunks.size(), [&](size_t I) {
    HashFn(Hashes.data() + I * HashSize, Chunks[I]);
  });

  // Write to the final output buffer.
  HashFn(HashBuf, Hashes);
}

BssSection::BssSection(StringRef Name)
    : SyntheticSection(SHF_ALLOC | SHF_WRITE, SHT_NOBITS, 0, Name) {}

size_t BssSection::reserveSpace(uint64_t Size, uint32_t Alignment) {
  if (OutSec)
    OutSec->updateAlignment(Alignment);
  this->Size = alignTo(this->Size, Alignment) + Size;
  this->Alignment = std::max(this->Alignment, Alignment);
  return this->Size - Size;
}

void BuildIdSection::writeBuildId(ArrayRef<uint8_t> Buf) {
  switch (Config->BuildId) {
  case BuildIdKind::Fast:
    computeHash(Buf, [](uint8_t *Dest, ArrayRef<uint8_t> Arr) {
      write64le(Dest, xxHash64(toStringRef(Arr)));
    });
    break;
  case BuildIdKind::Md5:
    computeHash(Buf, [](uint8_t *Dest, ArrayRef<uint8_t> Arr) {
      memcpy(Dest, MD5::hash(Arr).data(), 16);
    });
    break;
  case BuildIdKind::Sha1:
    computeHash(Buf, [](uint8_t *Dest, ArrayRef<uint8_t> Arr) {
      memcpy(Dest, SHA1::hash(Arr).data(), 20);
    });
    break;
  case BuildIdKind::Uuid:
    if (getRandomBytes(HashBuf, HashSize))
      error("entropy source failure");
    break;
  case BuildIdKind::Hexstring:
    memcpy(HashBuf, Config->BuildIdVector.data(), Config->BuildIdVector.size());
    break;
  default:
    llvm_unreachable("unknown BuildIdKind");
  }
}

template <class ELFT>
EhFrameSection<ELFT>::EhFrameSection()
    : SyntheticSection(SHF_ALLOC, SHT_PROGBITS, 1, ".eh_frame") {}

// Search for an existing CIE record or create a new one.
// CIE records from input object files are uniquified by their contents
// and where their relocations point to.
template <class ELFT>
template <class RelTy>
CieRecord *EhFrameSection<ELFT>::addCie(EhSectionPiece &Piece,
                                        ArrayRef<RelTy> Rels) {
  auto *Sec = cast<EhInputSection>(Piece.ID);
  const endianness E = ELFT::TargetEndianness;
  if (read32<E>(Piece.data().data() + 4) != 0)
    fatal(toString(Sec) + ": CIE expected at beginning of .eh_frame");

  SymbolBody *Personality = nullptr;
  unsigned FirstRelI = Piece.FirstRelocation;
  if (FirstRelI != (unsigned)-1)
    Personality =
        &Sec->template getFile<ELFT>()->getRelocTargetSym(Rels[FirstRelI]);

  // Search for an existing CIE by CIE contents/relocation target pair.
  CieRecord *Cie = &CieMap[{Piece.data(), Personality}];

  // If not found, create a new one.
  if (Cie->Piece == nullptr) {
    Cie->Piece = &Piece;
    Cies.push_back(Cie);
  }
  return Cie;
}

// There is one FDE per function. Returns true if a given FDE
// points to a live function.
template <class ELFT>
template <class RelTy>
bool EhFrameSection<ELFT>::isFdeLive(EhSectionPiece &Piece,
                                     ArrayRef<RelTy> Rels) {
  auto *Sec = cast<EhInputSection>(Piece.ID);
  unsigned FirstRelI = Piece.FirstRelocation;
  if (FirstRelI == (unsigned)-1)
    return false;
  const RelTy &Rel = Rels[FirstRelI];
  SymbolBody &B = Sec->template getFile<ELFT>()->getRelocTargetSym(Rel);
  auto *D = dyn_cast<DefinedRegular>(&B);
  if (!D || !D->Section)
    return false;
  auto *Target =
      cast<InputSectionBase>(cast<InputSectionBase>(D->Section)->Repl);
  return Target && Target->Live;
}

// .eh_frame is a sequence of CIE or FDE records. In general, there
// is one CIE record per input object file which is followed by
// a list of FDEs. This function searches an existing CIE or create a new
// one and associates FDEs to the CIE.
template <class ELFT>
template <class RelTy>
void EhFrameSection<ELFT>::addSectionAux(EhInputSection *Sec,
                                         ArrayRef<RelTy> Rels) {
  const endianness E = ELFT::TargetEndianness;

  DenseMap<size_t, CieRecord *> OffsetToCie;
  for (EhSectionPiece &Piece : Sec->Pieces) {
    // The empty record is the end marker.
    if (Piece.size() == 4)
      return;

    size_t Offset = Piece.InputOff;
    uint32_t ID = read32<E>(Piece.data().data() + 4);
    if (ID == 0) {
      OffsetToCie[Offset] = addCie(Piece, Rels);
      continue;
    }

    uint32_t CieOffset = Offset + 4 - ID;
    CieRecord *Cie = OffsetToCie[CieOffset];
    if (!Cie)
      fatal(toString(Sec) + ": invalid CIE reference");

    if (!isFdeLive(Piece, Rels))
      continue;
    Cie->FdePieces.push_back(&Piece);
    NumFdes++;
  }
}

template <class ELFT>
void EhFrameSection<ELFT>::addSection(InputSectionBase *C) {
  auto *Sec = cast<EhInputSection>(C);
  Sec->EHSec = this;
  updateAlignment(Sec->Alignment);
  Sections.push_back(Sec);
  for (auto *DS : Sec->DependentSections)
    DependentSections.push_back(DS);

  // .eh_frame is a sequence of CIE or FDE records. This function
  // splits it into pieces so that we can call
  // SplitInputSection::getSectionPiece on the section.
  Sec->split<ELFT>();
  if (Sec->Pieces.empty())
    return;

  if (Sec->NumRelocations) {
    if (Sec->AreRelocsRela)
      addSectionAux(Sec, Sec->template relas<ELFT>());
    else
      addSectionAux(Sec, Sec->template rels<ELFT>());
    return;
  }
  addSectionAux(Sec, makeArrayRef<Elf_Rela>(nullptr, nullptr));
}

template <class ELFT>
static void writeCieFde(uint8_t *Buf, ArrayRef<uint8_t> D) {
  memcpy(Buf, D.data(), D.size());

  // Fix the size field. -4 since size does not include the size field itself.
  const endianness E = ELFT::TargetEndianness;
  write32<E>(Buf, alignTo(D.size(), sizeof(typename ELFT::uint)) - 4);
}

template <class ELFT> void EhFrameSection<ELFT>::finalizeContents() {
  if (this->Size)
    return; // Already finalized.

  size_t Off = 0;
  for (CieRecord *Cie : Cies) {
    Cie->Piece->OutputOff = Off;
    Off += alignTo(Cie->Piece->size(), Config->Wordsize);

    for (EhSectionPiece *Fde : Cie->FdePieces) {
      Fde->OutputOff = Off;
      Off += alignTo(Fde->size(), Config->Wordsize);
    }
  }

  // The LSB standard does not allow a .eh_frame section with zero
  // Call Frame Information records. Therefore add a CIE record length
  // 0 as a terminator if this .eh_frame section is empty.
  if (Off == 0)
    Off = 4;

  this->Size = Off;
}

template <class ELFT> static uint64_t readFdeAddr(uint8_t *Buf, int Size) {
  const endianness E = ELFT::TargetEndianness;
  switch (Size) {
  case DW_EH_PE_udata2:
    return read16<E>(Buf);
  case DW_EH_PE_udata4:
    return read32<E>(Buf);
  case DW_EH_PE_udata8:
    return read64<E>(Buf);
  case DW_EH_PE_absptr:
    if (ELFT::Is64Bits)
      return read64<E>(Buf);
    return read32<E>(Buf);
  }
  fatal("unknown FDE size encoding");
}

// Returns the VA to which a given FDE (on a mmap'ed buffer) is applied to.
// We need it to create .eh_frame_hdr section.
template <class ELFT>
uint64_t EhFrameSection<ELFT>::getFdePc(uint8_t *Buf, size_t FdeOff,
                                        uint8_t Enc) {
  // The starting address to which this FDE applies is
  // stored at FDE + 8 byte.
  size_t Off = FdeOff + 8;
  uint64_t Addr = readFdeAddr<ELFT>(Buf + Off, Enc & 0x7);
  if ((Enc & 0x70) == DW_EH_PE_absptr)
    return Addr;
  if ((Enc & 0x70) == DW_EH_PE_pcrel)
    return Addr + this->OutSec->Addr + Off;
  fatal("unknown FDE size relative encoding");
}

template <class ELFT> void EhFrameSection<ELFT>::writeTo(uint8_t *Buf) {
  const endianness E = ELFT::TargetEndianness;
  for (CieRecord *Cie : Cies) {
    size_t CieOffset = Cie->Piece->OutputOff;
    writeCieFde<ELFT>(Buf + CieOffset, Cie->Piece->data());

    for (EhSectionPiece *Fde : Cie->FdePieces) {
      size_t Off = Fde->OutputOff;
      writeCieFde<ELFT>(Buf + Off, Fde->data());

      // FDE's second word should have the offset to an associated CIE.
      // Write it.
      write32<E>(Buf + Off + 4, Off + 4 - CieOffset);
    }
  }

  for (EhInputSection *S : Sections)
    S->relocateAlloc(Buf, nullptr);

  // Construct .eh_frame_hdr. .eh_frame_hdr is a binary search table
  // to get a FDE from an address to which FDE is applied. So here
  // we obtain two addresses and pass them to EhFrameHdr object.
  if (In<ELFT>::EhFrameHdr) {
    for (CieRecord *Cie : Cies) {
      uint8_t Enc = getFdeEncoding<ELFT>(Cie->Piece);
      for (SectionPiece *Fde : Cie->FdePieces) {
        uint64_t Pc = getFdePc(Buf, Fde->OutputOff, Enc);
        uint64_t FdeVA = this->OutSec->Addr + Fde->OutputOff;
        In<ELFT>::EhFrameHdr->addFde(Pc, FdeVA);
      }
    }
  }
}

GotSection::GotSection()
    : SyntheticSection(SHF_ALLOC | SHF_WRITE, SHT_PROGBITS,
                       Target->GotEntrySize, ".got") {}

void GotSection::addEntry(SymbolBody &Sym) {
  Sym.GotIndex = NumEntries;
  ++NumEntries;
}

bool GotSection::addDynTlsEntry(SymbolBody &Sym) {
  if (Sym.GlobalDynIndex != -1U)
    return false;
  Sym.GlobalDynIndex = NumEntries;
  // Global Dynamic TLS entries take two GOT slots.
  NumEntries += 2;
  return true;
}

// Reserves TLS entries for a TLS module ID and a TLS block offset.
// In total it takes two GOT slots.
bool GotSection::addTlsIndex() {
  if (TlsIndexOff != uint32_t(-1))
    return false;
  TlsIndexOff = NumEntries * Config->Wordsize;
  NumEntries += 2;
  return true;
}

uint64_t GotSection::getGlobalDynAddr(const SymbolBody &B) const {
  return this->getVA() + B.GlobalDynIndex * Config->Wordsize;
}

uint64_t GotSection::getGlobalDynOffset(const SymbolBody &B) const {
  return B.GlobalDynIndex * Config->Wordsize;
}

void GotSection::finalizeContents() { Size = NumEntries * Config->Wordsize; }

bool GotSection::empty() const {
  // If we have a relocation that is relative to GOT (such as GOTOFFREL),
  // we need to emit a GOT even if it's empty.
  return NumEntries == 0 && !HasGotOffRel;
}

void GotSection::writeTo(uint8_t *Buf) { relocateAlloc(Buf, Buf + Size); }

MipsGotSection::MipsGotSection()
    : SyntheticSection(SHF_ALLOC | SHF_WRITE | SHF_MIPS_GPREL, SHT_PROGBITS, 16,
                       ".got") {}

void MipsGotSection::addEntry(InputFile &File, SymbolBody &Sym, int64_t Addend,
                              RelExpr Expr) {
  FileGot &G = getGot(File);
  if (Expr == R_MIPS_GOT_LOCAL_PAGE) {
    auto *DefSym = cast<DefinedRegular>(&Sym);
    G.PageIndexMap.insert({DefSym->Section->getOutputSection(), 0});
  } else if (Sym.isTls())
    G.Tls.insert({&Sym, 0});
  else if (Sym.isPreemptible() && Expr == R_ABS)
    G.Relocs.insert({&Sym, 0});
  else if (Sym.isPreemptible())
    G.Global.insert({&Sym, 0});
  else if (Expr == R_MIPS_GOT_OFF32)
    G.Local32.insert({{&Sym, Addend}, 0});
  else
    G.Local16.insert({{&Sym, Addend}, 0});
}

void MipsGotSection::addDynTlsEntry(InputFile &File, SymbolBody &Sym) {
  getGot(File).DynTlsSymbols.insert({&Sym, 0});
}

void MipsGotSection::addTlsIndex(InputFile &File) {
  getGot(File).DynTlsSymbols.insert({nullptr, 0});
}

size_t MipsGotSection::FileGot::getEntriesNum() const {
  return getPageEntriesNum() + Local16.size() + Global.size() + Relocs.size() +
         Tls.size() + DynTlsSymbols.size() * 2;
}

static uint64_t getMipsPageAddr(uint64_t Addr) {
  return (Addr + 0x8000) & ~0xffff;
}

static uint64_t getMipsPageCount(uint64_t Size) {
  return (Size + 0xfffe) / 0xffff + 1;
}

size_t MipsGotSection::FileGot::getPageEntriesNum() const {
  size_t Num = 0;
  for (const std::pair<const OutputSection *, size_t> &P : PageIndexMap)
    Num += getMipsPageCount(P.first->Size);
  return Num;
}

size_t MipsGotSection::FileGot::getIndexEntriesNum() const {
  size_t Count = getPageEntriesNum() + Local16.size() + Global.size();
  // If there are relocation-only entries in the GOT, TLS entries
  // are allocated after them. TLS entries should be addressable
  // by 16-bit index so count both reloc-only and TLS entries.
  if (!Tls.empty() || !DynTlsSymbols.empty())
    Count += Relocs.size() + Tls.size() + DynTlsSymbols.size() * 2;
  return Count;
}

MipsGotSection::FileGot &MipsGotSection::getGot(InputFile &F) {
  if (F.MipsGotIndex == size_t(-1)) {
    Gots.emplace_back();
    Gots.back().File = &F;
    F.MipsGotIndex = Gots.size() - 1;
  }
  return Gots[F.MipsGotIndex];
}

uint64_t MipsGotSection::getPageEntryOffset(const InputFile &F,
                                            const SymbolBody &B,
                                            int64_t Addend) const {
  const FileGot &G = Gots[F.MipsGotIndex];
  const OutputSection *OutSec =
      cast<DefinedRegular>(&B)->Section->getOutputSection();
  uint64_t SecAddr = getMipsPageAddr(OutSec->Addr);
  uint64_t SymAddr = getMipsPageAddr(B.getVA(Addend));
  uint64_t Index = G.PageIndexMap.lookup(OutSec) + (SymAddr - SecAddr) / 0xffff;
  return Index * Config->Wordsize;
}

uint64_t MipsGotSection::getBodyEntryOffset(const InputFile &F,
                                            const SymbolBody &B,
                                            int64_t Addend) const {
  const FileGot &G = Gots[F.MipsGotIndex];
  SymbolBody *Body = const_cast<SymbolBody *>(&B);
  if (B.isTls()) {
    auto E = G.Tls.find(Body);
    assert(E != G.Tls.end());
    return E->second * Config->Wordsize;
  }
  if (B.isPreemptible()) {
    auto E = G.Global.find(Body);
    assert(E != G.Global.end());
    return E->second * Config->Wordsize;
  }
  auto E = G.Local16.find({Body, Addend});
  assert(E != G.Local16.end());
  return E->second * Config->Wordsize;
}

uint64_t MipsGotSection::getTlsIndexOffset(const InputFile &F) const {
  const FileGot &G = Gots[F.MipsGotIndex];
  auto E = G.DynTlsSymbols.find(nullptr);
  assert(E != G.DynTlsSymbols.end());
  return E->second * Config->Wordsize;
}

uint64_t MipsGotSection::getGlobalDynOffset(const InputFile &F,
                                            const SymbolBody &B) const {
  const FileGot &G = Gots[F.MipsGotIndex];
  auto E = G.DynTlsSymbols.find(const_cast<SymbolBody *>(&B));
  assert(E != G.DynTlsSymbols.end());
  return E->second * Config->Wordsize;
}

const SymbolBody *MipsGotSection::getFirstGlobalEntry() const {
  if (!Gots.empty()) {
    const FileGot &PrimGot = Gots.front();
    if (!PrimGot.Global.empty())
      return PrimGot.Global.front().first;
    if (!PrimGot.Relocs.empty())
      return PrimGot.Relocs.front().first;
  }
  return nullptr;
}

unsigned MipsGotSection::getLocalEntriesNum() const {
  if (Gots.empty())
    return HeaderEntriesNum;
  return HeaderEntriesNum + Gots.front().getPageEntriesNum() +
         Gots.front().Local16.size();
}

bool MipsGotSection::tryMergeGots(FileGot &Dst, FileGot &Src, bool IsPrimary) {
  FileGot Tmp = Dst;
  set_union(Tmp.PageIndexMap, Src.PageIndexMap);
  set_union(Tmp.Local16, Src.Local16);
  set_union(Tmp.Global, Src.Global);
  set_union(Tmp.Relocs, Src.Relocs);
  set_union(Tmp.Tls, Src.Tls);
  set_union(Tmp.DynTlsSymbols, Src.DynTlsSymbols);

  size_t Count = (IsPrimary ? HeaderEntriesNum : 0) + Tmp.getIndexEntriesNum();
  if (Count * Config->Wordsize > Config->MipsGotSize)
    return false;

  std::swap(Tmp, Dst);
  return true;
}

void MipsGotSection::finalizeContents() { updateAllocSize(); }

void MipsGotSection::updateAllocSize() {
  Size = HeaderEntriesNum * Config->Wordsize;
  for (const FileGot &G : Gots)
    Size += G.getEntriesNum() * Config->Wordsize;
}

template <class ELFT> void MipsGotSection::build() {
  if (Gots.empty())
    return;

  std::vector<FileGot> MergedGots(1);

  // For each GOT move non-preemptible symbols from the `Global`
  // to `Local16` list. Preemptible symbol might become non-preemptible
  // one if, for example, it gets a related copy relocation.
  for (FileGot &Got : Gots) {
    for (auto &P: Got.Global)
      if (!P.first->isPreemptible())
        Got.Local16.insert({{P.first, 0}, 0});
    Got.Global.remove_if([&](const std::pair<SymbolBody *, size_t> &P) {
      return !P.first->isPreemptible();
    });
  }

  // For each GOT remove "reloc-only" entry if there is "global"
  // entry for the same symbol. And add local entries which indexed
  // using 32-bit value at the end of 16-bit entries.
  for (FileGot &Got : Gots) {
    Got.Relocs.remove_if([&](const std::pair<SymbolBody *, size_t> &P) {
      return Got.Global.count(P.first);
    });
    set_union(Got.Local16, Got.Local32);
    Got.Local32.clear();
  }

  // Evaluate number of "reloc-only" entries in the resulting GOT.
  // To do that put all unique "reloc-only" and "global" entries
  // from all GOTs to the future primary GOT.
  FileGot *PrimGot = &MergedGots.front();
  for (FileGot &Got : Gots) {
    set_union(PrimGot->Relocs, Got.Global);
    set_union(PrimGot->Relocs, Got.Relocs);
    Got.Relocs.clear();
  }

  // Merge GOTs. Try to join as much as possible GOTs but do not
  // exceed maximum GOT size. In case of overflow create new GOT
  // and continue merging.
  for (FileGot &SrcGot : Gots) {
    FileGot &DstGot = MergedGots.back();
    InputFile *File = SrcGot.File;
    if (!tryMergeGots(DstGot, SrcGot, &DstGot == PrimGot)) {
      MergedGots.emplace_back();
      std::swap(MergedGots.back(), SrcGot);
    }
    File->MipsGotIndex = MergedGots.size() - 1;
  }
  std::swap(Gots, MergedGots);

  // Reduce number of "reloc-only" entries in the primary GOT
  // by substracting "global" entries exist in the primary GOT.
  PrimGot = &Gots.front();
  PrimGot->Relocs.remove_if([&](const std::pair<SymbolBody *, size_t> &P) {
    return PrimGot->Global.count(P.first);
  });

  // Calculate indexes for each GOT entry.
  size_t Index = HeaderEntriesNum;
  for (FileGot &Got : Gots) {
    Got.StartIndex = &Got == PrimGot ? 0 : Index;
    for (std::pair<const OutputSection *, size_t> &P : Got.PageIndexMap) {
      // For each output section referenced by GOT page relocations calculate
      // and save into PageIndexMap an upper bound of MIPS GOT entries required
      // to store page addresses of local symbols. We assume the worst case -
      // each 64kb page of the output section has at least one GOT relocation
      // against it. And take in account the case when the section intersects
      // page boundaries.
      P.second = Index;
      Index += getMipsPageCount(P.first->Size);
    }
    for (auto &P: Got.Local16)
      P.second = Index++;
    for (auto &P: Got.Global)
      P.second = Index++;
    for (auto &P: Got.Relocs)
      P.second = Index++;
    for (auto &P: Got.Tls)
      P.second = Index++;
    for (auto &P: Got.DynTlsSymbols) {
      P.second = Index;
      Index += 2;
    }
  }

  // Update SymbolBody::GotIndex field to use this
  // value later in the `sortMipsSymbols` function.
  for (auto &P : PrimGot->Global)
    P.first->GotIndex = P.second;
  for (auto &P : PrimGot->Relocs)
    P.first->GotIndex = P.second;

  // Create dynamic relocations.
  for (FileGot &Got : Gots) {
    // Create dynamic relocations for TLS entries.
    for (std::pair<SymbolBody *, size_t> &P : Got.Tls) {
      uint64_t Offset = P.second * Config->Wordsize;
      if (P.first->isPreemptible())
        In<ELFT>::RelaDyn->addReloc(
            {Target->TlsGotRel, this, Offset, false, P.first, 0});
    }
    for (std::pair<SymbolBody *, size_t> &P : Got.DynTlsSymbols) {
      uint64_t Offset = P.second * Config->Wordsize;
      if (P.first == nullptr) {
        if (!Config->Pic)
          continue;
        In<ELFT>::RelaDyn->addReloc(
            {Target->TlsModuleIndexRel, this, Offset, false, nullptr, 0});
      } else {
        if (!P.first->isPreemptible())
          continue;
        In<ELFT>::RelaDyn->addReloc(
            {Target->TlsModuleIndexRel, this, Offset, false, P.first, 0});
        Offset += Config->Wordsize;
        In<ELFT>::RelaDyn->addReloc(
            {Target->TlsOffsetRel, this, Offset, false, P.first, 0});
      }
    }

    // Do not create dynamic relocations for non-TLS
    // entries in the primary GOT.
    if (&Got == PrimGot)
      continue;

    // Dynamic relocations for "global" entries.
    for (const std::pair<SymbolBody *, size_t> &P : Got.Global) {
      uint64_t Offset = P.second * Config->Wordsize;
      In<ELFT>::RelaDyn->addReloc(
          {Target->RelativeRel, this, Offset, false, P.first, 0});
    }
    if (!Config->Pic)
      continue;
    // Dynamic relocations for "local" entries in case of PIC.
    for (const std::pair<const OutputSection *, size_t> &L : Got.PageIndexMap) {
      size_t PageCount = getMipsPageCount(L.first->Size);
      for (size_t PI = 0; PI < PageCount; ++PI) {
        uint64_t Offset = (L.second + PI) * Config->Wordsize;
        In<ELFT>::RelaDyn->addReloc({Target->RelativeRel, this, Offset, L.first,
                                     int64_t(PI * 0x10000)});
      }
    }
    for (const std::pair<GotEntry, size_t> &P : Got.Local16) {
      uint64_t Offset = P.second * Config->Wordsize;
      In<ELFT>::RelaDyn->addReloc({Target->RelativeRel, this, Offset, true,
                                   P.first.first, P.first.second});
    }
  }
}

bool MipsGotSection::empty() const {
  // We add the .got section to the result for dynamic MIPS target because
  // its address and properties are mentioned in the .dynamic section.
  return Config->Relocatable;
}

uint64_t MipsGotSection::getGp(const InputFile *F) const {
  if (!F || F->MipsGotIndex == 0 || F->MipsGotIndex == size_t(-1))
    return ElfSym::MipsGp->getVA(0);
  return this->getVA() + Gots[F->MipsGotIndex].StartIndex * Config->Wordsize +
         0x7ff0;
}

static uint64_t readUint(uint8_t *Buf) {
  if (Config->Is64)
    return read64(Buf, Config->Endianness);
  return read32(Buf, Config->Endianness);
}

static void writeUint(uint8_t *Buf, uint64_t Val) {
  if (Config->Is64)
    write64(Buf, Val, Config->Endianness);
  else
    write32(Buf, Val, Config->Endianness);
}

void MipsGotSection::writeTo(uint8_t *Buf) {
  // Set the MSB of the second GOT slot. This is not required by any
  // MIPS ABI documentation, though.
  //
  // There is a comment in glibc saying that "The MSB of got[1] of a
  // gnu object is set to identify gnu objects," and in GNU gold it
  // says "the second entry will be used by some runtime loaders".
  // But how this field is being used is unclear.
  //
  // We are not really willing to mimic other linkers behaviors
  // without understanding why they do that, but because all files
  // generated by GNU tools have this special GOT value, and because
  // we've been doing this for years, it is probably a safe bet to
  // keep doing this for now. We really need to revisit this to see
  // if we had to do this.
  writeUint(Buf + Config->Wordsize, (uint64_t)1 << (Config->Wordsize * 8 - 1));
  for (const FileGot &G : Gots) {
    // Write 'page address' entries to the local part of the GOT.
    for (const std::pair<const OutputSection *, size_t> &L : G.PageIndexMap) {
      size_t PageCount = getMipsPageCount(L.first->Size);
      uint64_t FirstPageAddr = getMipsPageAddr(L.first->Addr);
      for (size_t PI = 0; PI < PageCount; ++PI) {
        uint8_t *Addr = Buf + (L.second + PI) * Config->Wordsize;
        writeUint(Addr, FirstPageAddr + PI * 0x10000);
      }
    }
    // Local, global, TLS, reloc-only  entries.
    // If TLS entry has a corresponding dynamic relocations, leave it
    // initialized by zero. Write down adjusted TLS symbol's values otherwise.
    // To calculate the adjustments use offsets for thread-local storage.
    // https://www.linux-mips.org/wiki/NPTL
    for (const std::pair<GotEntry, size_t> &P : G.Local16) {
      uint8_t *Addr = Buf + P.second * Config->Wordsize;
      writeUint(Addr, P.first.first->getVA(P.first.second));
    }
    // Write VA to the primary GOT only. For secondary GOTs that
    // will be done by REL32 dynamic relocations.
    if (&G == &Gots.front()) {
      for (const std::pair<const SymbolBody *, size_t> &P : G.Global) {
        uint8_t *Addr = Buf + P.second * Config->Wordsize;
        writeUint(Addr, P.first->getVA(0));
      }
    }
    for (const std::pair<SymbolBody *, size_t> &P : G.Relocs) {
      uint8_t *Addr = Buf + P.second * Config->Wordsize;
      writeUint(Addr, P.first->getVA(0));
    }
    for (const std::pair<SymbolBody *, size_t> &P : G.Tls) {
      uint8_t *Addr = Buf + P.second * Config->Wordsize;
      uint64_t VA = P.first->getVA();
      writeUint(Addr, P.first->isPreemptible() ? VA : VA - 0x7000);
    }
    for (const std::pair<SymbolBody *, size_t> &P : G.DynTlsSymbols) {
      if (P.first == nullptr) {
        if (!Config->Pic)
          writeUint(Buf + P.second * Config->Wordsize, 1);
      } else {
        if (!P.first->isPreemptible()) {
          uint8_t *Addr = Buf + P.second * Config->Wordsize;
          writeUint(Addr, 1);
          writeUint(Addr + Config->Wordsize, P.first->getVA() - 0x8000);
        }
      }
    }
  }
}

GotPltSection::GotPltSection()
    : SyntheticSection(SHF_ALLOC | SHF_WRITE, SHT_PROGBITS,
                       Target->GotPltEntrySize, ".got.plt") {}

void GotPltSection::addEntry(SymbolBody &Sym) {
  Sym.GotPltIndex = Target->GotPltHeaderEntriesNum + Entries.size();
  Entries.push_back(&Sym);
}

size_t GotPltSection::getSize() const {
  return (Target->GotPltHeaderEntriesNum + Entries.size()) *
         Target->GotPltEntrySize;
}

void GotPltSection::writeTo(uint8_t *Buf) {
  Target->writeGotPltHeader(Buf);
  Buf += Target->GotPltHeaderEntriesNum * Target->GotPltEntrySize;
  for (const SymbolBody *B : Entries) {
    Target->writeGotPlt(Buf, *B);
    Buf += Config->Wordsize;
  }
}

// On ARM the IgotPltSection is part of the GotSection, on other Targets it is
// part of the .got.plt
IgotPltSection::IgotPltSection()
    : SyntheticSection(SHF_ALLOC | SHF_WRITE, SHT_PROGBITS,
                       Target->GotPltEntrySize,
                       Config->EMachine == EM_ARM ? ".got" : ".got.plt") {}

void IgotPltSection::addEntry(SymbolBody &Sym) {
  Sym.IsInIgot = true;
  Sym.GotPltIndex = Entries.size();
  Entries.push_back(&Sym);
}

size_t IgotPltSection::getSize() const {
  return Entries.size() * Target->GotPltEntrySize;
}

void IgotPltSection::writeTo(uint8_t *Buf) {
  for (const SymbolBody *B : Entries) {
    Target->writeIgotPlt(Buf, *B);
    Buf += Config->Wordsize;
  }
}

StringTableSection::StringTableSection(StringRef Name, bool Dynamic)
    : SyntheticSection(Dynamic ? (uint64_t)SHF_ALLOC : 0, SHT_STRTAB, 1, Name),
      Dynamic(Dynamic) {
  // ELF string tables start with a NUL byte.
  addString("");
}

// Adds a string to the string table. If HashIt is true we hash and check for
// duplicates. It is optional because the name of global symbols are already
// uniqued and hashing them again has a big cost for a small value: uniquing
// them with some other string that happens to be the same.
unsigned StringTableSection::addString(StringRef S, bool HashIt) {
  if (HashIt) {
    auto R = StringMap.insert(std::make_pair(S, this->Size));
    if (!R.second)
      return R.first->second;
  }
  unsigned Ret = this->Size;
  this->Size = this->Size + S.size() + 1;
  Strings.push_back(S);
  return Ret;
}

void StringTableSection::writeTo(uint8_t *Buf) {
  for (StringRef S : Strings) {
    memcpy(Buf, S.data(), S.size());
    Buf += S.size() + 1;
  }
}

// Returns the number of version definition entries. Because the first entry
// is for the version definition itself, it is the number of versioned symbols
// plus one. Note that we don't support multiple versions yet.
static unsigned getVerDefNum() { return Config->VersionDefinitions.size() + 1; }

template <class ELFT>
DynamicSection<ELFT>::DynamicSection()
    : SyntheticSection(SHF_ALLOC | SHF_WRITE, SHT_DYNAMIC, Config->Wordsize,
                       ".dynamic") {
  this->Entsize = ELFT::Is64Bits ? 16 : 8;

  // .dynamic section is not writable on MIPS and on Fuchsia OS
  // which passes -z rodynamic.
  // See "Special Section" in Chapter 4 in the following document:
  // ftp://www.linux-mips.org/pub/linux/mips/doc/ABI/mipsabi.pdf
  if (Config->EMachine == EM_MIPS || Config->ZRodynamic)
    this->Flags = SHF_ALLOC;

  addEntries();
}

// There are some dynamic entries that don't depend on other sections.
// Such entries can be set early.
template <class ELFT> void DynamicSection<ELFT>::addEntries() {
  // Add strings to .dynstr early so that .dynstr's size will be
  // fixed early.
  for (StringRef S : Config->AuxiliaryList)
    add({DT_AUXILIARY, InX::DynStrTab->addString(S)});
  if (!Config->Rpath.empty())
    add({Config->EnableNewDtags ? DT_RUNPATH : DT_RPATH,
         InX::DynStrTab->addString(Config->Rpath)});
  for (SharedFile<ELFT> *F : Symtab<ELFT>::X->getSharedFiles())
    if (F->isNeeded())
      add({DT_NEEDED, InX::DynStrTab->addString(F->SoName)});
  if (!Config->SoName.empty())
    add({DT_SONAME, InX::DynStrTab->addString(Config->SoName)});

  // Set DT_FLAGS and DT_FLAGS_1.
  uint32_t DtFlags = 0;
  uint32_t DtFlags1 = 0;
  if (Config->Bsymbolic)
    DtFlags |= DF_SYMBOLIC;
  if (Config->ZNodelete)
    DtFlags1 |= DF_1_NODELETE;
  if (Config->ZNodlopen)
    DtFlags1 |= DF_1_NOOPEN;
  if (Config->ZNow) {
    DtFlags |= DF_BIND_NOW;
    DtFlags1 |= DF_1_NOW;
  }
  if (Config->ZOrigin) {
    DtFlags |= DF_ORIGIN;
    DtFlags1 |= DF_1_ORIGIN;
  }

  if (DtFlags)
    add({DT_FLAGS, DtFlags});
  if (DtFlags1)
    add({DT_FLAGS_1, DtFlags1});

  // DT_DEBUG is a pointer to debug informaion used by debuggers at runtime. We
  // need it for each process, so we don't write it for DSOs. The loader writes
  // the pointer into this entry.
  //
  // DT_DEBUG is the only .dynamic entry that needs to be written to. Some
  // systems (currently only Fuchsia OS) provide other means to give the
  // debugger this information. Such systems may choose make .dynamic read-only.
  // If the target is such a system (used -z rodynamic) don't write DT_DEBUG.
  if (!Config->Shared && !Config->Relocatable && !Config->ZRodynamic)
    add({DT_DEBUG, (uint64_t)0});
}

// Add remaining entries to complete .dynamic contents.
template <class ELFT> void DynamicSection<ELFT>::finalizeContents() {
  if (this->Size)
    return; // Already finalized.

  this->Link = InX::DynStrTab->OutSec->SectionIndex;
  if (In<ELFT>::RelaDyn->OutSec->Size > 0) {
    bool IsRela = Config->IsRela;
    add({IsRela ? DT_RELA : DT_REL, In<ELFT>::RelaDyn});
    add({IsRela ? DT_RELASZ : DT_RELSZ, In<ELFT>::RelaDyn->OutSec->Size});
    add({IsRela ? DT_RELAENT : DT_RELENT,
         uint64_t(IsRela ? sizeof(Elf_Rela) : sizeof(Elf_Rel))});

    // MIPS dynamic loader does not support RELCOUNT tag.
    // The problem is in the tight relation between dynamic
    // relocations and GOT. So do not emit this tag on MIPS.
    if (Config->EMachine != EM_MIPS) {
      size_t NumRelativeRels = In<ELFT>::RelaDyn->getRelativeRelocCount();
      if (Config->ZCombreloc && NumRelativeRels)
        add({IsRela ? DT_RELACOUNT : DT_RELCOUNT, NumRelativeRels});
    }
  }
  if (In<ELFT>::RelaPlt->OutSec->Size > 0) {
    add({DT_JMPREL, In<ELFT>::RelaPlt});
    add({DT_PLTRELSZ, In<ELFT>::RelaPlt->OutSec->Size});
    add({Config->EMachine == EM_MIPS ? DT_MIPS_PLTGOT : DT_PLTGOT,
         InX::GotPlt});
    add({DT_PLTREL, uint64_t(Config->IsRela ? DT_RELA : DT_REL)});
  }

  add({DT_SYMTAB, InX::DynSymTab});
  add({DT_SYMENT, sizeof(Elf_Sym)});
  add({DT_STRTAB, InX::DynStrTab});
  add({DT_STRSZ, InX::DynStrTab->getSize()});
  if (!Config->ZText)
    add({DT_TEXTREL, (uint64_t)0});
  if (InX::GnuHashTab)
    add({DT_GNU_HASH, InX::GnuHashTab});
  if (In<ELFT>::HashTab)
    add({DT_HASH, In<ELFT>::HashTab});

  if (Out::PreinitArray) {
    add({DT_PREINIT_ARRAY, Out::PreinitArray});
    add({DT_PREINIT_ARRAYSZ, Out::PreinitArray, Entry::SecSize});
  }
  if (Out::InitArray) {
    add({DT_INIT_ARRAY, Out::InitArray});
    add({DT_INIT_ARRAYSZ, Out::InitArray, Entry::SecSize});
  }
  if (Out::FiniArray) {
    add({DT_FINI_ARRAY, Out::FiniArray});
    add({DT_FINI_ARRAYSZ, Out::FiniArray, Entry::SecSize});
  }

  if (SymbolBody *B = Symtab<ELFT>::X->findInCurrentDSO(Config->Init))
    add({DT_INIT, B});
  if (SymbolBody *B = Symtab<ELFT>::X->findInCurrentDSO(Config->Fini))
    add({DT_FINI, B});

  bool HasVerNeed = In<ELFT>::VerNeed->getNeedNum() != 0;
  if (HasVerNeed || In<ELFT>::VerDef)
    add({DT_VERSYM, In<ELFT>::VerSym});
  if (In<ELFT>::VerDef) {
    add({DT_VERDEF, In<ELFT>::VerDef});
    add({DT_VERDEFNUM, getVerDefNum()});
  }
  if (HasVerNeed) {
    add({DT_VERNEED, In<ELFT>::VerNeed});
    add({DT_VERNEEDNUM, In<ELFT>::VerNeed->getNeedNum()});
  }

  if (Config->EMachine == EM_MIPS) {
    add({DT_MIPS_RLD_VERSION, 1});
    add({DT_MIPS_FLAGS, RHF_NOTPOT});
    add({DT_MIPS_BASE_ADDRESS, Config->ImageBase});
    add({DT_MIPS_SYMTABNO, InX::DynSymTab->getNumSymbols()});
    add({DT_MIPS_LOCAL_GOTNO, InX::MipsGot->getLocalEntriesNum()});
    if (const SymbolBody *B = InX::MipsGot->getFirstGlobalEntry())
      add({DT_MIPS_GOTSYM, B->DynsymIndex});
    else
      add({DT_MIPS_GOTSYM, InX::DynSymTab->getNumSymbols()});
    add({DT_PLTGOT, InX::MipsGot});
    if (InX::MipsRldMap)
      add({DT_MIPS_RLD_MAP, InX::MipsRldMap});
  }

  this->OutSec->Link = this->Link;

  // +1 for DT_NULL
  this->Size = (Entries.size() + 1) * this->Entsize;
}

template <class ELFT> void DynamicSection<ELFT>::writeTo(uint8_t *Buf) {
  auto *P = reinterpret_cast<Elf_Dyn *>(Buf);

  for (const Entry &E : Entries) {
    P->d_tag = E.Tag;
    switch (E.Kind) {
    case Entry::SecAddr:
      P->d_un.d_ptr = E.OutSec->Addr;
      break;
    case Entry::InSecAddr:
      P->d_un.d_ptr = E.InSec->OutSec->Addr + E.InSec->OutSecOff;
      break;
    case Entry::SecSize:
      P->d_un.d_val = E.OutSec->Size;
      break;
    case Entry::SymAddr:
      P->d_un.d_ptr = E.Sym->getVA();
      break;
    case Entry::PlainInt:
      P->d_un.d_val = E.Val;
      break;
    }
    ++P;
  }
  // write out the trailing DT_NULL entry
  P->d_tag = DT_NULL;
  P->d_un.d_ptr = 0;
}

uint64_t DynamicReloc::getOffset() const {
  return InputSec->getOutputSection()->Addr + InputSec->getOffset(OffsetInSec);
}

int64_t DynamicReloc::getAddend() const {
  if (UseSymVA)
    return Sym->getVA(Addend);
  if (!OutputSec)
    return Addend;
  // See the comment in the DynamicReloc ctor.
  return getMipsPageAddr(OutputSec->Addr) + Addend;
}

uint32_t DynamicReloc::getSymIndex() const {
  if (Sym && !UseSymVA)
    return Sym->DynsymIndex;
  return 0;
}

template <class ELFT>
RelocationSection<ELFT>::RelocationSection(StringRef Name, bool Sort)
    : SyntheticSection(SHF_ALLOC, Config->IsRela ? SHT_RELA : SHT_REL,
                       Config->Wordsize, Name),
      Sort(Sort) {
  this->Entsize = Config->IsRela ? sizeof(Elf_Rela) : sizeof(Elf_Rel);
}

template <class ELFT>
void RelocationSection<ELFT>::addReloc(const DynamicReloc &Reloc) {
  if (Reloc.Type == Target->RelativeRel)
    ++NumRelativeRelocs;
  Relocs.push_back(Reloc);
  auto IS = Reloc.getInputSec();
  if (!Config->IsRela && IS->AreRelocsRela) {
    // HACK for FreeBSD mips n64/CHERI: input is RELA, output is REL -> write the addend to the output
    const_cast<InputSectionBase*>(IS)->FreeBSDMipsRelocationsHack.push_back(Reloc);
  }
}

template <class ELFT, class RelTy>
static bool compRelocations(const RelTy &A, const RelTy &B) {
  bool AIsRel = A.getType(Config->IsMips64EL) == Target->RelativeRel;
  bool BIsRel = B.getType(Config->IsMips64EL) == Target->RelativeRel;
  if (AIsRel != BIsRel)
    return AIsRel;

  return A.getSymbol(Config->IsMips64EL) < B.getSymbol(Config->IsMips64EL);
}

template <class ELFT> void RelocationSection<ELFT>::writeTo(uint8_t *Buf) {
  uint8_t *BufBegin = Buf;
  for (const DynamicReloc &Rel : Relocs) {
    auto *P = reinterpret_cast<Elf_Rela *>(Buf);
    Buf += Config->IsRela ? sizeof(Elf_Rela) : sizeof(Elf_Rel);

    if (Config->IsRela)
      P->r_addend = Rel.getAddend();
    P->r_offset = Rel.getOffset();
    P->setSymbolAndType(Rel.getSymIndex(), Rel.Type, Config->IsMips64EL);
  }

  if (Sort) {
    if (Config->IsRela)
      std::stable_sort((Elf_Rela *)BufBegin,
                       (Elf_Rela *)BufBegin + Relocs.size(),
                       compRelocations<ELFT, Elf_Rela>);
    else
      std::stable_sort((Elf_Rel *)BufBegin, (Elf_Rel *)BufBegin + Relocs.size(),
                       compRelocations<ELFT, Elf_Rel>);
  }
}

template <class ELFT> unsigned RelocationSection<ELFT>::getRelocOffset() {
  return this->Entsize * Relocs.size();
}

template <class ELFT> void RelocationSection<ELFT>::finalizeContents() {
  this->Link = InX::DynSymTab ? InX::DynSymTab->OutSec->SectionIndex
                              : InX::SymTab->OutSec->SectionIndex;

  // Set required output section properties.
  this->OutSec->Link = this->Link;
}

SymbolTableBaseSection::SymbolTableBaseSection(StringTableSection &StrTabSec)
    : SyntheticSection(StrTabSec.isDynamic() ? (uint64_t)SHF_ALLOC : 0,
                       StrTabSec.isDynamic() ? SHT_DYNSYM : SHT_SYMTAB,
                       Config->Wordsize,
                       StrTabSec.isDynamic() ? ".dynsym" : ".symtab"),
      StrTabSec(StrTabSec) {}

// Orders symbols according to their positions in the GOT,
// in compliance with MIPS ABI rules.
// See "Global Offset Table" in Chapter 5 in the following document
// for detailed description:
// ftp://www.linux-mips.org/pub/linux/mips/doc/ABI/mipsabi.pdf
static bool sortMipsSymbols(const SymbolTableEntry &L,
                            const SymbolTableEntry &R) {
  // Sort entries related to non-local preemptible symbols by GOT indexes.
  // All other entries go to the first part of GOT in arbitrary order.
  if (!L.Symbol->isInGot() || !R.Symbol->isInGot())
    return !L.Symbol->isInGot();
  return L.Symbol->GotIndex < R.Symbol->GotIndex;
}

// Finalize a symbol table. The ELF spec requires that all local
// symbols precede global symbols, so we sort symbol entries in this
// function. (For .dynsym, we don't do that because symbols for
// dynamic linking are inherently all globals.)
void SymbolTableBaseSection::finalizeContents() {
  this->OutSec->Link = StrTabSec.OutSec->SectionIndex;

  // If it is a .dynsym, there should be no local symbols, but we need
  // to do a few things for the dynamic linker.
  if (this->Type == SHT_DYNSYM) {
    // Section's Info field has the index of the first non-local symbol.
    // Because the first symbol entry is a null entry, 1 is the first.
    this->OutSec->Info = 1;

    if (InX::GnuHashTab) {
      // NB: It also sorts Symbols to meet the GNU hash table requirements.
      InX::GnuHashTab->addSymbols(Symbols);
    } else if (Config->EMachine == EM_MIPS) {
      std::stable_sort(Symbols.begin(), Symbols.end(), sortMipsSymbols);
    }

    size_t I = 0;
    for (const SymbolTableEntry &S : Symbols)
      S.Symbol->DynsymIndex = ++I;
    return;
  }
}

void SymbolTableBaseSection::postThunkContents() {
  if (this->Type == SHT_DYNSYM)
    return;
  // move all local symbols before global symbols.
  auto It = std::stable_partition(
      Symbols.begin(), Symbols.end(), [](const SymbolTableEntry &S) {
        return S.Symbol->isLocal() ||
               S.Symbol->symbol()->computeBinding() == STB_LOCAL;
      });
  size_t NumLocals = It - Symbols.begin();
  this->OutSec->Info = NumLocals + 1;
}

void SymbolTableBaseSection::addSymbol(SymbolBody *B) {
  // Adding a local symbol to a .dynsym is a bug.
  assert(this->Type != SHT_DYNSYM || !B->isLocal());

  bool HashIt = B->isLocal();
  Symbols.push_back({B, StrTabSec.addString(B->getName(), HashIt)});
}

size_t SymbolTableBaseSection::getSymbolIndex(SymbolBody *Body) {
  auto I = llvm::find_if(Symbols, [&](const SymbolTableEntry &E) {
    if (E.Symbol == Body)
      return true;
    // This is used for -r, so we have to handle multiple section
    // symbols being combined.
    if (Body->Type == STT_SECTION && E.Symbol->Type == STT_SECTION)
      return cast<DefinedRegular>(Body)->Section->getOutputSection() ==
             cast<DefinedRegular>(E.Symbol)->Section->getOutputSection();
    return false;
  });
  if (I == Symbols.end())
    return 0;
  return I - Symbols.begin() + 1;
}

template <class ELFT>
SymbolTableSection<ELFT>::SymbolTableSection(StringTableSection &StrTabSec)
    : SymbolTableBaseSection(StrTabSec) {
  this->Entsize = sizeof(Elf_Sym);
}

// Write the internal symbol table contents to the output symbol table.
template <class ELFT> void SymbolTableSection<ELFT>::writeTo(uint8_t *Buf) {
  // The first entry is a null entry as per the ELF spec.
  Buf += sizeof(Elf_Sym);

  auto *ESym = reinterpret_cast<Elf_Sym *>(Buf);

  for (SymbolTableEntry &Ent : Symbols) {
    SymbolBody *Body = Ent.Symbol;

    // Set st_info and st_other.
    if (Body->isLocal()) {
      ESym->setBindingAndType(STB_LOCAL, Body->Type);
    } else {
      ESym->setBindingAndType(Body->symbol()->computeBinding(), Body->Type);
      ESym->setVisibility(Body->symbol()->Visibility);
    }

    ESym->st_name = Ent.StrTabOffset;
    ESym->st_size = Body->getSize<ELFT>();

    // Set a section index.
    if (const OutputSection *OutSec = Body->getOutputSection())
      ESym->st_shndx = OutSec->SectionIndex;
    else if (isa<DefinedRegular>(Body))
      ESym->st_shndx = SHN_ABS;
    else if (isa<DefinedCommon>(Body))
      ESym->st_shndx = SHN_COMMON;

    // st_value is usually an address of a symbol, but that has a
    // special meaining for uninstantiated common symbols (this can
    // occur if -r is given).
    if (!Config->DefineCommon && isa<DefinedCommon>(Body))
      ESym->st_value = cast<DefinedCommon>(Body)->Alignment;
    else
      ESym->st_value = Body->getVA();

    ++ESym;
  }

  // On MIPS we need to mark symbol which has a PLT entry and requires
  // pointer equality by STO_MIPS_PLT flag. That is necessary to help
  // dynamic linker distinguish such symbols and MIPS lazy-binding stubs.
  // https://sourceware.org/ml/binutils/2008-07/txt00000.txt
  if (Config->EMachine == EM_MIPS) {
    auto *ESym = reinterpret_cast<Elf_Sym *>(Buf);

    for (SymbolTableEntry &Ent : Symbols) {
      SymbolBody *Body = Ent.Symbol;
      if (Body->isInPlt() && Body->NeedsPltAddr)
        ESym->st_other |= STO_MIPS_PLT;

      if (Config->Relocatable)
        if (auto *D = dyn_cast<DefinedRegular>(Body))
          if (D->isMipsPIC<ELFT>())
            ESym->st_other |= STO_MIPS_PIC;
      ++ESym;
    }
  }
}

// .hash and .gnu.hash sections contain on-disk hash tables that map
// symbol names to their dynamic symbol table indices. Their purpose
// is to help the dynamic linker resolve symbols quickly. If ELF files
// don't have them, the dynamic linker has to do linear search on all
// dynamic symbols, which makes programs slower. Therefore, a .hash
// section is added to a DSO by default. A .gnu.hash is added if you
// give the -hash-style=gnu or -hash-style=both option.
//
// The Unix semantics of resolving dynamic symbols is somewhat expensive.
// Each ELF file has a list of DSOs that the ELF file depends on and a
// list of dynamic symbols that need to be resolved from any of the
// DSOs. That means resolving all dynamic symbols takes O(m)*O(n)
// where m is the number of DSOs and n is the number of dynamic
// symbols. For modern large programs, both m and n are large.  So
// making each step faster by using hash tables substiantially
// improves time to load programs.
//
// (Note that this is not the only way to design the shared library.
// For instance, the Windows DLL takes a different approach. On
// Windows, each dynamic symbol has a name of DLL from which the symbol
// has to be resolved. That makes the cost of symbol resolution O(n).
// This disables some hacky techniques you can use on Unix such as
// LD_PRELOAD, but this is arguably better semantics than the Unix ones.)
//
// Due to historical reasons, we have two different hash tables, .hash
// and .gnu.hash. They are for the same purpose, and .gnu.hash is a new
// and better version of .hash. .hash is just an on-disk hash table, but
// .gnu.hash has a bloom filter in addition to a hash table to skip
// DSOs very quickly. If you are sure that your dynamic linker knows
// about .gnu.hash, you want to specify -hash-style=gnu. Otherwise, a
// safe bet is to specify -hash-style=both for backward compatibilty.
GnuHashTableSection::GnuHashTableSection()
    : SyntheticSection(SHF_ALLOC, SHT_GNU_HASH, Config->Wordsize, ".gnu.hash") {
}

void GnuHashTableSection::finalizeContents() {
  this->OutSec->Link = InX::DynSymTab->OutSec->SectionIndex;

  // Computes bloom filter size in word size. We want to allocate 8
  // bits for each symbol. It must be a power of two.
  if (Symbols.empty())
    MaskWords = 1;
  else
    MaskWords = NextPowerOf2((Symbols.size() - 1) / Config->Wordsize);

  Size = 16;                            // Header
  Size += Config->Wordsize * MaskWords; // Bloom filter
  Size += NBuckets * 4;                 // Hash buckets
  Size += Symbols.size() * 4;           // Hash values
}

void GnuHashTableSection::writeTo(uint8_t *Buf) {
  // Write a header.
  write32(Buf, NBuckets, Config->Endianness);
  write32(Buf + 4, InX::DynSymTab->getNumSymbols() - Symbols.size(),
          Config->Endianness);
  write32(Buf + 8, MaskWords, Config->Endianness);
  write32(Buf + 12, getShift2(), Config->Endianness);
  Buf += 16;

  // Write a bloom filter and a hash table.
  writeBloomFilter(Buf);
  Buf += Config->Wordsize * MaskWords;
  writeHashTable(Buf);
}

// This function writes a 2-bit bloom filter. This bloom filter alone
// usually filters out 80% or more of all symbol lookups [1].
// The dynamic linker uses the hash table only when a symbol is not
// filtered out by a bloom filter.
//
// [1] Ulrich Drepper (2011), "How To Write Shared Libraries" (Ver. 4.1.2),
//     p.9, https://www.akkadia.org/drepper/dsohowto.pdf
void GnuHashTableSection::writeBloomFilter(uint8_t *Buf) {
  const unsigned C = Config->Wordsize * 8;
  for (const Entry &Sym : Symbols) {
    size_t I = (Sym.Hash / C) & (MaskWords - 1);
    uint64_t Val = readUint(Buf + I * Config->Wordsize);
    Val |= uint64_t(1) << (Sym.Hash % C);
    Val |= uint64_t(1) << ((Sym.Hash >> getShift2()) % C);
    writeUint(Buf + I * Config->Wordsize, Val);
  }
}

void GnuHashTableSection::writeHashTable(uint8_t *Buf) {
  // Group symbols by hash value.
  std::vector<std::vector<Entry>> Syms(NBuckets);
  for (const Entry &Ent : Symbols)
    Syms[Ent.Hash % NBuckets].push_back(Ent);

  // Write hash buckets. Hash buckets contain indices in the following
  // hash value table.
  uint32_t *Buckets = reinterpret_cast<uint32_t *>(Buf);
  for (size_t I = 0; I < NBuckets; ++I)
    if (!Syms[I].empty())
      write32(Buckets + I, Syms[I][0].Body->DynsymIndex, Config->Endianness);

  // Write a hash value table. It represents a sequence of chains that
  // share the same hash modulo value. The last element of each chain
  // is terminated by LSB 1.
  uint32_t *Values = Buckets + NBuckets;
  size_t I = 0;
  for (std::vector<Entry> &Vec : Syms) {
    if (Vec.empty())
      continue;
    for (const Entry &Ent : makeArrayRef(Vec).drop_back())
      write32(Values + I++, Ent.Hash & ~1, Config->Endianness);
    write32(Values + I++, Vec.back().Hash | 1, Config->Endianness);
  }
}

static uint32_t hashGnu(StringRef Name) {
  uint32_t H = 5381;
  for (uint8_t C : Name)
    H = (H << 5) + H + C;
  return H;
}

// Returns a number of hash buckets to accomodate given number of elements.
// We want to choose a moderate number that is not too small (which
// causes too many hash collisions) and not too large (which wastes
// disk space.)
//
// We return a prime number because it (is believed to) achieve good
// hash distribution.
static size_t getBucketSize(size_t NumSymbols) {
  // List of largest prime numbers that are not greater than 2^n + 1.
  for (size_t N : {131071, 65521, 32749, 16381, 8191, 4093, 2039, 1021, 509,
                   251, 127, 61, 31, 13, 7, 3, 1})
    if (N <= NumSymbols)
      return N;
  return 0;
}

// Add symbols to this symbol hash table. Note that this function
// destructively sort a given vector -- which is needed because
// GNU-style hash table places some sorting requirements.
void GnuHashTableSection::addSymbols(std::vector<SymbolTableEntry> &V) {
  // We cannot use 'auto' for Mid because GCC 6.1 cannot deduce
  // its type correctly.
  std::vector<SymbolTableEntry>::iterator Mid =
      std::stable_partition(V.begin(), V.end(), [](const SymbolTableEntry &S) {
        return S.Symbol->isUndefined();
      });
  if (Mid == V.end())
    return;

  for (SymbolTableEntry &Ent : llvm::make_range(Mid, V.end())) {
    SymbolBody *B = Ent.Symbol;
    Symbols.push_back({B, Ent.StrTabOffset, hashGnu(B->getName())});
  }

  NBuckets = getBucketSize(Symbols.size());
  std::stable_sort(Symbols.begin(), Symbols.end(),
                   [&](const Entry &L, const Entry &R) {
                     return L.Hash % NBuckets < R.Hash % NBuckets;
                   });

  V.erase(Mid, V.end());
  for (const Entry &Ent : Symbols)
    V.push_back({Ent.Body, Ent.StrTabOffset});
}

template <class ELFT>
HashTableSection<ELFT>::HashTableSection()
    : SyntheticSection(SHF_ALLOC, SHT_HASH, 4, ".hash") {
  this->Entsize = 4;
}

template <class ELFT> void HashTableSection<ELFT>::finalizeContents() {
  this->OutSec->Link = InX::DynSymTab->OutSec->SectionIndex;

  unsigned NumEntries = 2;                            // nbucket and nchain.
  NumEntries += InX::DynSymTab->getNumSymbols(); // The chain entries.

  // Create as many buckets as there are symbols.
  // FIXME: This is simplistic. We can try to optimize it, but implementing
  // support for SHT_GNU_HASH is probably even more profitable.
  NumEntries += InX::DynSymTab->getNumSymbols();
  this->Size = NumEntries * 4;
}

template <class ELFT> void HashTableSection<ELFT>::writeTo(uint8_t *Buf) {
  // A 32-bit integer type in the target endianness.
  typedef typename ELFT::Word Elf_Word;

  unsigned NumSymbols = InX::DynSymTab->getNumSymbols();

  auto *P = reinterpret_cast<Elf_Word *>(Buf);
  *P++ = NumSymbols; // nbucket
  *P++ = NumSymbols; // nchain

  Elf_Word *Buckets = P;
  Elf_Word *Chains = P + NumSymbols;

  for (const SymbolTableEntry &S : InX::DynSymTab->getSymbols()) {
    SymbolBody *Body = S.Symbol;
    StringRef Name = Body->getName();
    unsigned I = Body->DynsymIndex;
    uint32_t Hash = hashSysV(Name) % NumSymbols;
    Chains[I] = Buckets[Hash];
    Buckets[Hash] = I;
  }
}

PltSection::PltSection(size_t S)
    : SyntheticSection(SHF_ALLOC | SHF_EXECINSTR, SHT_PROGBITS, 16, ".plt"),
      HeaderSize(S) {}

void PltSection::writeTo(uint8_t *Buf) {
  // At beginning of PLT but not the IPLT, we have code to call the dynamic
  // linker to resolve dynsyms at runtime. Write such code.
  if (HeaderSize != 0)
    Target->writePltHeader(Buf);
  size_t Off = HeaderSize;
  // The IPlt is immediately after the Plt, account for this in RelOff
  unsigned PltOff = getPltRelocOff();

  for (auto &I : Entries) {
    const SymbolBody *B = I.first;
    unsigned RelOff = I.second + PltOff;
    uint64_t Got = B->getGotPltVA();
    uint64_t Plt = this->getVA() + Off;
    Target->writePlt(Buf + Off, Got, Plt, B->PltIndex, RelOff);
    Off += Target->PltEntrySize;
  }
}

template <class ELFT> void PltSection::addEntry(SymbolBody &Sym) {
  Sym.PltIndex = Entries.size();
  RelocationSection<ELFT> *PltRelocSection = In<ELFT>::RelaPlt;
  if (HeaderSize == 0) {
    PltRelocSection = In<ELFT>::RelaIplt;
    Sym.IsInIplt = true;
  }
  unsigned RelOff = PltRelocSection->getRelocOffset();
  Entries.push_back(std::make_pair(&Sym, RelOff));
}

size_t PltSection::getSize() const {
  return HeaderSize + Entries.size() * Target->PltEntrySize;
}

// Some architectures such as additional symbols in the PLT section. For
// example ARM uses mapping symbols to aid disassembly
void PltSection::addSymbols() {
  // The PLT may have symbols defined for the Header, the IPLT has no header
  if (HeaderSize != 0)
    Target->addPltHeaderSymbols(this);
  size_t Off = HeaderSize;
  for (size_t I = 0; I < Entries.size(); ++I) {
    Target->addPltSymbols(this, Off);
    Off += Target->PltEntrySize;
  }
}

unsigned PltSection::getPltRelocOff() const {
  return (HeaderSize == 0) ? InX::Plt->getSize() : 0;
}

GdbIndexSection::GdbIndexSection()
    : SyntheticSection(0, SHT_PROGBITS, 1, ".gdb_index"),
      StringPool(llvm::StringTableBuilder::ELF) {}

// Iterative hash function for symbol's name is described in .gdb_index format
// specification. Note that we use one for version 5 to 7 here, it is different
// for version 4.
static uint32_t hash(StringRef Str) {
  uint32_t R = 0;
  for (uint8_t C : Str)
    R = R * 67 + tolower(C) - 113;
  return R;
}

static std::vector<std::pair<uint64_t, uint64_t>>
readCuList(DWARFContext &Dwarf, InputSection *Sec) {
  std::vector<std::pair<uint64_t, uint64_t>> Ret;
  for (std::unique_ptr<DWARFCompileUnit> &CU : Dwarf.compile_units())
    Ret.push_back({Sec->OutSecOff + CU->getOffset(), CU->getLength() + 4});
  return Ret;
}

static InputSection *findSection(ArrayRef<InputSectionBase *> Arr,
                                 uint64_t Offset) {
  for (InputSectionBase *S : Arr)
    if (auto *IS = dyn_cast_or_null<InputSection>(S))
      if (IS != &InputSection::Discarded && IS->Live &&
          Offset >= IS->getOffsetInFile() &&
          Offset < IS->getOffsetInFile() + IS->getSize())
        return IS;
  return nullptr;
}

static std::vector<AddressEntry>
readAddressArea(DWARFContext &Dwarf, InputSection *Sec, size_t CurrentCU) {
  std::vector<AddressEntry> Ret;

  for (std::unique_ptr<DWARFCompileUnit> &CU : Dwarf.compile_units()) {
    DWARFAddressRangesVector Ranges;
    CU->collectAddressRanges(Ranges);

    ArrayRef<InputSectionBase *> Sections = Sec->File->getSections();
    for (DWARFAddressRange &R : Ranges)
      if (InputSection *S = findSection(Sections, R.LowPC))
        Ret.push_back({S, R.LowPC - S->getOffsetInFile(),
                       R.HighPC - S->getOffsetInFile(), CurrentCU});
    ++CurrentCU;
  }
  return Ret;
}

static std::vector<std::pair<StringRef, uint8_t>>
readPubNamesAndTypes(DWARFContext &Dwarf, bool IsLE) {
  StringRef Data[] = {Dwarf.getGnuPubNamesSection(),
                      Dwarf.getGnuPubTypesSection()};

  std::vector<std::pair<StringRef, uint8_t>> Ret;
  for (StringRef D : Data) {
    DWARFDebugPubTable PubTable(D, IsLE, true);
    for (const DWARFDebugPubTable::Set &Set : PubTable.getData())
      for (const DWARFDebugPubTable::Entry &Ent : Set.Entries)
        Ret.push_back({Ent.Name, Ent.Descriptor.toBits()});
  }
  return Ret;
}

class ObjInfoTy : public llvm::LoadedObjectInfo {
  uint64_t getSectionLoadAddress(const object::SectionRef &Sec) const override {
    auto &S = static_cast<const object::ELFSectionRef &>(Sec);
    if (S.getFlags() & ELF::SHF_ALLOC)
      return S.getOffset();
    return 0;
  }

  std::unique_ptr<llvm::LoadedObjectInfo> clone() const override { return {}; }
};

void GdbIndexSection::readDwarf(InputSection *Sec) {
  Expected<std::unique_ptr<object::ObjectFile>> Obj =
      object::ObjectFile::createObjectFile(Sec->File->MB);
  if (!Obj) {
    error(toString(Sec->File) + ": error creating DWARF context");
    return;
  }

  ObjInfoTy ObjInfo;
  DWARFContextInMemory Dwarf(*Obj.get(), &ObjInfo);

  size_t CuId = CompilationUnits.size();
  for (std::pair<uint64_t, uint64_t> &P : readCuList(Dwarf, Sec))
    CompilationUnits.push_back(P);

  for (AddressEntry &Ent : readAddressArea(Dwarf, Sec, CuId))
    AddressArea.push_back(Ent);

  std::vector<std::pair<StringRef, uint8_t>> NamesAndTypes =
      readPubNamesAndTypes(Dwarf, Config->IsLE);

  for (std::pair<StringRef, uint8_t> &Pair : NamesAndTypes) {
    uint32_t Hash = hash(Pair.first);
    size_t Offset = StringPool.add(Pair.first);

    bool IsNew;
    GdbSymbol *Sym;
    std::tie(IsNew, Sym) = SymbolTable.add(Hash, Offset);
    if (IsNew) {
      Sym->CuVectorIndex = CuVectors.size();
      CuVectors.resize(CuVectors.size() + 1);
    }

    CuVectors[Sym->CuVectorIndex].insert((Pair.second << 24) | (uint32_t)CuId);
  }
}

void GdbIndexSection::finalizeContents() {
  if (Finalized)
    return;
  Finalized = true;

  for (InputSectionBase *S : InputSections)
    if (InputSection *IS = dyn_cast<InputSection>(S))
      if (IS->OutSec && IS->Name == ".debug_info")
        readDwarf(IS);

  SymbolTable.finalizeContents();

  // GdbIndex header consist from version fields
  // and 5 more fields with different kinds of offsets.
  CuTypesOffset = CuListOffset + CompilationUnits.size() * CompilationUnitSize;
  SymTabOffset = CuTypesOffset + AddressArea.size() * AddressEntrySize;

  ConstantPoolOffset =
      SymTabOffset + SymbolTable.getCapacity() * SymTabEntrySize;

  for (std::set<uint32_t> &CuVec : CuVectors) {
    CuVectorsOffset.push_back(CuVectorsSize);
    CuVectorsSize += OffsetTypeSize * (CuVec.size() + 1);
  }
  StringPoolOffset = ConstantPoolOffset + CuVectorsSize;

  StringPool.finalizeInOrder();
}

size_t GdbIndexSection::getSize() const {
  const_cast<GdbIndexSection *>(this)->finalizeContents();
  return StringPoolOffset + StringPool.getSize();
}

void GdbIndexSection::writeTo(uint8_t *Buf) {
  write32le(Buf, 7);                       // Write version.
  write32le(Buf + 4, CuListOffset);        // CU list offset.
  write32le(Buf + 8, CuTypesOffset);       // Types CU list offset.
  write32le(Buf + 12, CuTypesOffset);      // Address area offset.
  write32le(Buf + 16, SymTabOffset);       // Symbol table offset.
  write32le(Buf + 20, ConstantPoolOffset); // Constant pool offset.
  Buf += 24;

  // Write the CU list.
  for (std::pair<uint64_t, uint64_t> CU : CompilationUnits) {
    write64le(Buf, CU.first);
    write64le(Buf + 8, CU.second);
    Buf += 16;
  }

  // Write the address area.
  for (AddressEntry &E : AddressArea) {
    uint64_t BaseAddr = E.Section->OutSec->Addr + E.Section->getOffset(0);
    write64le(Buf, BaseAddr + E.LowAddress);
    write64le(Buf + 8, BaseAddr + E.HighAddress);
    write32le(Buf + 16, E.CuIndex);
    Buf += 20;
  }

  // Write the symbol table.
  for (size_t I = 0; I < SymbolTable.getCapacity(); ++I) {
    GdbSymbol *Sym = SymbolTable.getSymbol(I);
    if (Sym) {
      size_t NameOffset =
          Sym->NameOffset + StringPoolOffset - ConstantPoolOffset;
      size_t CuVectorOffset = CuVectorsOffset[Sym->CuVectorIndex];
      write32le(Buf, NameOffset);
      write32le(Buf + 4, CuVectorOffset);
    }
    Buf += 8;
  }

  // Write the CU vectors into the constant pool.
  for (std::set<uint32_t> &CuVec : CuVectors) {
    write32le(Buf, CuVec.size());
    Buf += 4;
    for (uint32_t Val : CuVec) {
      write32le(Buf, Val);
      Buf += 4;
    }
  }

  StringPool.write(Buf);
}

bool GdbIndexSection::empty() const {
  return !Out::DebugInfo;
}

template <class ELFT>
EhFrameHeader<ELFT>::EhFrameHeader()
    : SyntheticSection(SHF_ALLOC, SHT_PROGBITS, 1, ".eh_frame_hdr") {}

// .eh_frame_hdr contains a binary search table of pointers to FDEs.
// Each entry of the search table consists of two values,
// the starting PC from where FDEs covers, and the FDE's address.
// It is sorted by PC.
template <class ELFT> void EhFrameHeader<ELFT>::writeTo(uint8_t *Buf) {
  const endianness E = ELFT::TargetEndianness;

  // Sort the FDE list by their PC and uniqueify. Usually there is only
  // one FDE for a PC (i.e. function), but if ICF merges two functions
  // into one, there can be more than one FDEs pointing to the address.
  auto Less = [](const FdeData &A, const FdeData &B) { return A.Pc < B.Pc; };
  std::stable_sort(Fdes.begin(), Fdes.end(), Less);
  auto Eq = [](const FdeData &A, const FdeData &B) { return A.Pc == B.Pc; };
  Fdes.erase(std::unique(Fdes.begin(), Fdes.end(), Eq), Fdes.end());

  Buf[0] = 1;
  Buf[1] = DW_EH_PE_pcrel | DW_EH_PE_sdata4;
  Buf[2] = DW_EH_PE_udata4;
  Buf[3] = DW_EH_PE_datarel | DW_EH_PE_sdata4;
  write32<E>(Buf + 4, In<ELFT>::EhFrame->OutSec->Addr - this->getVA() - 4);
  write32<E>(Buf + 8, Fdes.size());
  Buf += 12;

  uint64_t VA = this->getVA();
  for (FdeData &Fde : Fdes) {
    write32<E>(Buf, Fde.Pc - VA);
    write32<E>(Buf + 4, Fde.FdeVA - VA);
    Buf += 8;
  }
}

template <class ELFT> size_t EhFrameHeader<ELFT>::getSize() const {
  // .eh_frame_hdr has a 12 bytes header followed by an array of FDEs.
  return 12 + In<ELFT>::EhFrame->NumFdes * 8;
}

template <class ELFT>
void EhFrameHeader<ELFT>::addFde(uint32_t Pc, uint32_t FdeVA) {
  Fdes.push_back({Pc, FdeVA});
}

template <class ELFT> bool EhFrameHeader<ELFT>::empty() const {
  return In<ELFT>::EhFrame->empty();
}

template <class ELFT>
VersionDefinitionSection<ELFT>::VersionDefinitionSection()
    : SyntheticSection(SHF_ALLOC, SHT_GNU_verdef, sizeof(uint32_t),
                       ".gnu.version_d") {}

static StringRef getFileDefName() {
  if (!Config->SoName.empty())
    return Config->SoName;
  return Config->OutputFile;
}

template <class ELFT> void VersionDefinitionSection<ELFT>::finalizeContents() {
  FileDefNameOff = InX::DynStrTab->addString(getFileDefName());
  for (VersionDefinition &V : Config->VersionDefinitions)
    V.NameOff = InX::DynStrTab->addString(V.Name);

  this->OutSec->Link = InX::DynStrTab->OutSec->SectionIndex;

  // sh_info should be set to the number of definitions. This fact is missed in
  // documentation, but confirmed by binutils community:
  // https://sourceware.org/ml/binutils/2014-11/msg00355.html
  this->OutSec->Info = getVerDefNum();
}

template <class ELFT>
void VersionDefinitionSection<ELFT>::writeOne(uint8_t *Buf, uint32_t Index,
                                              StringRef Name, size_t NameOff) {
  auto *Verdef = reinterpret_cast<Elf_Verdef *>(Buf);
  Verdef->vd_version = 1;
  Verdef->vd_cnt = 1;
  Verdef->vd_aux = sizeof(Elf_Verdef);
  Verdef->vd_next = sizeof(Elf_Verdef) + sizeof(Elf_Verdaux);
  Verdef->vd_flags = (Index == 1 ? VER_FLG_BASE : 0);
  Verdef->vd_ndx = Index;
  Verdef->vd_hash = hashSysV(Name);

  auto *Verdaux = reinterpret_cast<Elf_Verdaux *>(Buf + sizeof(Elf_Verdef));
  Verdaux->vda_name = NameOff;
  Verdaux->vda_next = 0;
}

template <class ELFT>
void VersionDefinitionSection<ELFT>::writeTo(uint8_t *Buf) {
  writeOne(Buf, 1, getFileDefName(), FileDefNameOff);

  for (VersionDefinition &V : Config->VersionDefinitions) {
    Buf += sizeof(Elf_Verdef) + sizeof(Elf_Verdaux);
    writeOne(Buf, V.Id, V.Name, V.NameOff);
  }

  // Need to terminate the last version definition.
  Elf_Verdef *Verdef = reinterpret_cast<Elf_Verdef *>(Buf);
  Verdef->vd_next = 0;
}

template <class ELFT> size_t VersionDefinitionSection<ELFT>::getSize() const {
  return (sizeof(Elf_Verdef) + sizeof(Elf_Verdaux)) * getVerDefNum();
}

template <class ELFT>
VersionTableSection<ELFT>::VersionTableSection()
    : SyntheticSection(SHF_ALLOC, SHT_GNU_versym, sizeof(uint16_t),
                       ".gnu.version") {
  this->Entsize = sizeof(Elf_Versym);
}

template <class ELFT> void VersionTableSection<ELFT>::finalizeContents() {
  // At the moment of june 2016 GNU docs does not mention that sh_link field
  // should be set, but Sun docs do. Also readelf relies on this field.
  this->OutSec->Link = InX::DynSymTab->OutSec->SectionIndex;
}

template <class ELFT> size_t VersionTableSection<ELFT>::getSize() const {
  return sizeof(Elf_Versym) * (InX::DynSymTab->getSymbols().size() + 1);
}

template <class ELFT> void VersionTableSection<ELFT>::writeTo(uint8_t *Buf) {
  auto *OutVersym = reinterpret_cast<Elf_Versym *>(Buf) + 1;
  for (const SymbolTableEntry &S : InX::DynSymTab->getSymbols()) {
    OutVersym->vs_index = S.Symbol->symbol()->VersionId;
    ++OutVersym;
  }
}

template <class ELFT> bool VersionTableSection<ELFT>::empty() const {
  return !In<ELFT>::VerDef && In<ELFT>::VerNeed->empty();
}

template <class ELFT>
VersionNeedSection<ELFT>::VersionNeedSection()
    : SyntheticSection(SHF_ALLOC, SHT_GNU_verneed, sizeof(uint32_t),
                       ".gnu.version_r") {
  // Identifiers in verneed section start at 2 because 0 and 1 are reserved
  // for VER_NDX_LOCAL and VER_NDX_GLOBAL.
  // First identifiers are reserved by verdef section if it exist.
  NextIndex = getVerDefNum() + 1;
}

template <class ELFT>
void VersionNeedSection<ELFT>::addSymbol(SharedSymbol *SS) {
  auto *Ver = reinterpret_cast<const typename ELFT::Verdef *>(SS->Verdef);
  if (!Ver) {
    SS->symbol()->VersionId = VER_NDX_GLOBAL;
    return;
  }

  auto *File = cast<SharedFile<ELFT>>(SS->File);

  // If we don't already know that we need an Elf_Verneed for this DSO, prepare
  // to create one by adding it to our needed list and creating a dynstr entry
  // for the soname.
  if (File->VerdefMap.empty())
    Needed.push_back({File, InX::DynStrTab->addString(File->SoName)});
  typename SharedFile<ELFT>::NeededVer &NV = File->VerdefMap[Ver];
  // If we don't already know that we need an Elf_Vernaux for this Elf_Verdef,
  // prepare to create one by allocating a version identifier and creating a
  // dynstr entry for the version name.
  if (NV.Index == 0) {
    NV.StrTab = InX::DynStrTab->addString(File->getStringTable().data() +
                                          Ver->getAux()->vda_name);
    NV.Index = NextIndex++;
  }
  SS->symbol()->VersionId = NV.Index;
}

template <class ELFT> void VersionNeedSection<ELFT>::writeTo(uint8_t *Buf) {
  // The Elf_Verneeds need to appear first, followed by the Elf_Vernauxs.
  auto *Verneed = reinterpret_cast<Elf_Verneed *>(Buf);
  auto *Vernaux = reinterpret_cast<Elf_Vernaux *>(Verneed + Needed.size());

  for (std::pair<SharedFile<ELFT> *, size_t> &P : Needed) {
    // Create an Elf_Verneed for this DSO.
    Verneed->vn_version = 1;
    Verneed->vn_cnt = P.first->VerdefMap.size();
    Verneed->vn_file = P.second;
    Verneed->vn_aux =
        reinterpret_cast<char *>(Vernaux) - reinterpret_cast<char *>(Verneed);
    Verneed->vn_next = sizeof(Elf_Verneed);
    ++Verneed;

    // Create the Elf_Vernauxs for this Elf_Verneed. The loop iterates over
    // VerdefMap, which will only contain references to needed version
    // definitions. Each Elf_Vernaux is based on the information contained in
    // the Elf_Verdef in the source DSO. This loop iterates over a std::map of
    // pointers, but is deterministic because the pointers refer to Elf_Verdef
    // data structures within a single input file.
    for (auto &NV : P.first->VerdefMap) {
      Vernaux->vna_hash = NV.first->vd_hash;
      Vernaux->vna_flags = 0;
      Vernaux->vna_other = NV.second.Index;
      Vernaux->vna_name = NV.second.StrTab;
      Vernaux->vna_next = sizeof(Elf_Vernaux);
      ++Vernaux;
    }

    Vernaux[-1].vna_next = 0;
  }
  Verneed[-1].vn_next = 0;
}

template <class ELFT> void VersionNeedSection<ELFT>::finalizeContents() {
  this->OutSec->Link = InX::DynStrTab->OutSec->SectionIndex;
  this->OutSec->Info = Needed.size();
}

template <class ELFT> size_t VersionNeedSection<ELFT>::getSize() const {
  unsigned Size = Needed.size() * sizeof(Elf_Verneed);
  for (const std::pair<SharedFile<ELFT> *, size_t> &P : Needed)
    Size += P.first->VerdefMap.size() * sizeof(Elf_Vernaux);
  return Size;
}

template <class ELFT> bool VersionNeedSection<ELFT>::empty() const {
  return getNeedNum() == 0;
}

MergeSyntheticSection::MergeSyntheticSection(StringRef Name, uint32_t Type,
                                             uint64_t Flags, uint32_t Alignment)
    : SyntheticSection(Flags, Type, Alignment, Name),
      Builder(StringTableBuilder::RAW, Alignment) {}

void MergeSyntheticSection::addSection(MergeInputSection *MS) {
  assert(!Finalized);
  MS->MergeSec = this;
  Sections.push_back(MS);
}

void MergeSyntheticSection::writeTo(uint8_t *Buf) { Builder.write(Buf); }

bool MergeSyntheticSection::shouldTailMerge() const {
  return (this->Flags & SHF_STRINGS) && Config->Optimize >= 2;
}

void MergeSyntheticSection::finalizeTailMerge() {
  // Add all string pieces to the string table builder to create section
  // contents.
  for (MergeInputSection *Sec : Sections)
    for (size_t I = 0, E = Sec->Pieces.size(); I != E; ++I)
      if (Sec->Pieces[I].Live)
        Builder.add(Sec->getData(I));

  // Fix the string table content. After this, the contents will never change.
  Builder.finalize();

  // finalize() fixed tail-optimized strings, so we can now get
  // offsets of strings. Get an offset for each string and save it
  // to a corresponding StringPiece for easy access.
  for (MergeInputSection *Sec : Sections)
    for (size_t I = 0, E = Sec->Pieces.size(); I != E; ++I)
      if (Sec->Pieces[I].Live)
        Sec->Pieces[I].OutputOff = Builder.getOffset(Sec->getData(I));
}

void MergeSyntheticSection::finalizeNoTailMerge() {
  // Add all string pieces to the string table builder to create section
  // contents. Because we are not tail-optimizing, offsets of strings are
  // fixed when they are added to the builder (string table builder contains
  // a hash table from strings to offsets).
  for (MergeInputSection *Sec : Sections)
    for (size_t I = 0, E = Sec->Pieces.size(); I != E; ++I)
      if (Sec->Pieces[I].Live)
        Sec->Pieces[I].OutputOff = Builder.add(Sec->getData(I));

  Builder.finalizeInOrder();
}

void MergeSyntheticSection::finalizeContents() {
  if (Finalized)
    return;
  Finalized = true;
  if (shouldTailMerge())
    finalizeTailMerge();
  else
    finalizeNoTailMerge();
}

size_t MergeSyntheticSection::getSize() const {
  // We should finalize string builder to know the size.
  const_cast<MergeSyntheticSection *>(this)->finalizeContents();
  return Builder.getSize();
}

MipsRldMapSection::MipsRldMapSection()
    : SyntheticSection(SHF_ALLOC | SHF_WRITE, SHT_PROGBITS, Config->Wordsize,
                       ".rld_map") {}

ARMExidxSentinelSection::ARMExidxSentinelSection()
    : SyntheticSection(SHF_ALLOC | SHF_LINK_ORDER, SHT_ARM_EXIDX,
                       Config->Wordsize, ".ARM.exidx") {}

// Write a terminating sentinel entry to the end of the .ARM.exidx table.
// This section will have been sorted last in the .ARM.exidx table.
// This table entry will have the form:
// | PREL31 upper bound of code that has exception tables | EXIDX_CANTUNWIND |
// The sentinel must have the PREL31 value of an address higher than any
// address described by any other table entry.
void ARMExidxSentinelSection::writeTo(uint8_t *Buf) {
  // The Sections are sorted in order of ascending PREL31 address with the
  // sentinel last. We need to find the InputSection that precedes the
  // sentinel. By construction the Sentinel is in the last
  // InputSectionDescription as the InputSection that precedes it.
  OutputSectionCommand *C = Script->getCmd(OutSec);
  auto ISD = std::find_if(C->Commands.rbegin(), C->Commands.rend(),
                          [](const BaseCommand *Base) {
                            return isa<InputSectionDescription>(Base);
                          });
  auto L = cast<InputSectionDescription>(*ISD);
  InputSection *Highest = L->Sections[L->Sections.size() - 2];
  InputSection *LS = cast<InputSection>(Highest->getLinkOrderDep());
  uint64_t S = LS->OutSec->Addr + LS->getOffset(LS->getSize());
  uint64_t P = getVA();
  Target->relocateOne(Buf, R_ARM_PREL31, S - P);
  write32le(Buf + 4, 0x1);
}

ThunkSection::ThunkSection(OutputSection *OS, uint64_t Off)
    : SyntheticSection(SHF_ALLOC | SHF_EXECINSTR, SHT_PROGBITS,
                       Config->Wordsize, ".text.thunk") {
  this->OutSec = OS;
  this->OutSecOff = Off;
}

void ThunkSection::addThunk(Thunk *T) {
  uint64_t Off = alignTo(Size, T->alignment);
  T->Offset = Off;
  Thunks.push_back(T);
  T->addSymbols(*this);
  Size = Off + T->size();
}

void ThunkSection::writeTo(uint8_t *Buf) {
  for (const Thunk *T : Thunks)
    T->writeTo(Buf + T->Offset, *this);
}

InputSection *ThunkSection::getTargetInputSection() const {
  const Thunk *T = Thunks.front();
  return T->getTargetInputSection();
}

template <class ELFT>
CheriCapRelocsSection<ELFT>::CheriCapRelocsSection()
    : SyntheticSection(SHF_ALLOC, SHT_PROGBITS, 8, "__cap_relocs") {
  this->Entsize = RelocSize;
}

// TODO: copy MipsABIFlagsSection::create() instead of current impl?
template <class ELFT>
void CheriCapRelocsSection<ELFT>::addSection(InputSectionBase *S) {
  assert(S->Name == "__cap_relocs");
  assert(S->AreRelocsRela && "__cap_relocs should be RELA");
  // make sure the section is no longer processed
  S->OutSec = nullptr;
  S->Live = false;

  if ((S->getSize() % Entsize) != 0) {
      error("__cap_relocs section size is not a multiple of " + Twine(Entsize) +
            ": " + toString(S));
      return;
  }
  size_t NumCapRelocs = S->getSize() / RelocSize;
  if (NumCapRelocs * 2 != S->NumRelocations) {
    error("expected " + Twine(NumCapRelocs * 2) + " relocations for " + toString(S) +
          " but got " + Twine(S->NumRelocations));
    return;
  }
  if (Config->VerboseCapRelocs)
    message("Adding cap relocs from " + toString(S->File) + "\n");

  processSection(S);
}

template <class ELFT>
void CheriCapRelocsSection<ELFT>::finalizeContents() {
  // TODO: sort by address for improved cache behaviour?
  // TODO: add the dynamic relocations here:
//   for (const auto &I : RelocsMap) {
//     // TODO: unresolved symbols -> add dynamic reloc
//     const CheriCapReloc& Reloc = I.second;
//     SymbolBody *LocationSym = I.first.first;
//     uint64_t LocationOffset = I.first.second;
//
//   }
}

template <class ELFT>
static std::string verboseToString(SymbolBody *B, uint64_t SymOffset = 0) {
  std::string Msg;

  if (B->isLocal())
    Msg += "local ";
  if (B->isShared())
    Msg += "shared ";
  if (B->isCommon())
    Msg += "common ";
  if (B->isSection())
    Msg += "section ";
  else if (B->isTls())
    Msg += "tls ";
  else if (B->isFunc())
    Msg += "function ";
  else if (B->isGnuIFunc())
    Msg += "gnu ifunc ";
  else if (B->isObject())
    Msg += "object ";
  else if (B->isFile())
    Msg += "object ";
  else
    Msg += "<unknown kind>";

  if (B->isInCurrentDSO())
    Msg += "(in current DSO) ";
  if (B->NeedsCopy)
    Msg += "(needs copy) ";
  if (B->isInGot())
    Msg += "(in GOT) ";
  if (B->isInPlt())
    Msg += "(in PLT) ";

  std::string Name = toString(*B);
  DefinedRegular* DR = dyn_cast<DefinedRegular>(B);
  InputSectionBase* IS = nullptr;
  if (Name.empty()) {
    if (DR && DR->Section) {
      InputSectionBase* IS = dyn_cast<InputSectionBase>(DR->Section);
      auto Offset = DR->isSection() ? SymOffset : DR->Section->getOffset(*DR);
      if (IS) {
        Name = IS->getLocation<ELFT>(Offset);
      } else {
        Name = (DR->Section->Name + "+0x" + utohexstr(Offset)).str();
      }
    } else if (OutputSection* OS = B->getOutputSection()) {
      Name = (OS->Name + "+(unknown offset)").str();
    }
  }
  if (Name.empty()) {
    Name = "<unknown symbol>";
  }
  Msg += Name;
  std::string Src = IS ? IS->getSrcMsg<ELFT>(SymOffset) : toString(B->File);
  Msg += "\n>>> defined in " + Src;
  return Msg;
}

template <class ELFT>
static std::string getCapRelocSource(const CheriCapRelocLocation& Src,
                                     const CheriCapReloc& Reloc) {
  // return "against " + verboseToString<ELFT>(Reloc.Target, Reloc.Offset) +
  return "against " + verboseToString<ELFT>(Reloc.Target, Reloc.TargetSymbolOffset) +
         "\n>>> referenced by " + verboseToString<ELFT>(Src.BaseSym, Src.Offset);
}

template<typename ELFT>
static std::pair<DefinedRegular*, uint64_t> sectionWithOffsetToSymbol(InputSectionBase* IS, uint64_t Offset) {
  DefinedRegular* FallbackResult = nullptr;
  uint64_t FallbackOffset = Offset;
  // llvm::errs() << "Sectionoffset: " << IS->getLocation<ELFT>(Offset) << "\n";
  for (SymbolBody *B : IS->getFile<ELFT>()->getSymbols()) {
    if (auto *D = dyn_cast<DefinedRegular>(B)) {
      if (D->Section != IS)
        continue;
      if (D->Value <= Offset && Offset < D->Value + D->Size) {
        // XXXAR: should we accept any symbol that encloses or only exact matches?
        if (D->Value == Offset && (D->isFunc() || D->isObject()))
          return std::make_pair(D, D->Value - Offset); // perfect match
        FallbackResult = D;
        FallbackOffset = Offset - D->Value;
      }
    }
  }
  // we should have found at least a section symbol
  assert(FallbackResult && "SHOULD HAVE FOUND A SYMBOL!");
  return std::make_pair(FallbackResult, FallbackOffset);
}

template <class ELFT>
void elf::CheriCapRelocsSection<ELFT>::processSection(InputSectionBase *S) {
  constexpr endianness E = ELFT::TargetEndianness;
  // TODO: sort by offset (or is that always true?
  const auto Rels = S->relas<ELFT>();
  for (auto I = Rels.begin(), End = Rels.end(); I != End; ++I) {
    const auto& LocationRel = *I;
    ++I;
    const auto& TargetRel = *I;
    if ((LocationRel.r_offset % Entsize) != 0) {
      error("corrupted __cap_relocs:  expected Relocation offset to be a "
                    "multiple of " + Twine(Entsize) + " but got " + Twine(LocationRel.r_offset));
      return;
    }
    if (TargetRel.r_offset != LocationRel.r_offset + 8) {
      error("corrupted __cap_relocs: expected target relocation (" +
            Twine(TargetRel.r_offset) + " to directly follow location relocation (" +
            Twine(LocationRel.r_offset) + ")");
      return;
    }
    if (LocationRel.r_addend < 0) {
      error("corrupted __cap_relocs: addend is less than zero in" +
            toString(S) + ": " + Twine(LocationRel.r_addend));
      return;
    }
    uint64_t CapRelocsOffset = LocationRel.r_offset;
    assert(CapRelocsOffset + Entsize <= S->getSize());
    if (LocationRel.getType(Config->IsMips64EL) != R_MIPS_64) {
      error("Exptected a R_MIPS_64 relocation in __cap_relocs but got " +
            toString(LocationRel.getType(Config->IsMips64EL)));
      continue;
    }
    if (TargetRel.getType(Config->IsMips64EL) != R_MIPS_64) {
      error("Exptected a R_MIPS_64 relocation in __cap_relocs but got " +
            toString(LocationRel.getType(Config->IsMips64EL)));
      continue;
    }
    SymbolBody *LocationSym = &S->getFile<ELFT>()->getRelocTargetSym(LocationRel);
    SymbolBody &TargetSym = S->getFile<ELFT>()->getRelocTargetSym(TargetRel);

    if (LocationSym->File != S->File) {
      error("Expected capability relocation to point to " + toString(S->File) +
            " but got " + toString(LocationSym->File));
      continue;
    }
//    errs() << "Adding cap reloc at " << toString(LocationSym) << " type "
//           << Twine((int)LocationSym.Type) << " against "
//           << toString(TargetSym) << "\n";
    const uint64_t LocationOffset = LocationRel.r_addend;
    const uint64_t TargetOffset = TargetRel.r_addend;
    auto *RawInput = reinterpret_cast<const InMemoryCapRelocEntry<E>*>(
            S->Data.begin() + CapRelocsOffset);
    bool LocNeedsDynReloc = false;
    std::pair<DefinedRegular*, uint64_t> RealLocation;
    InputSectionBase* SourceSection = nullptr; // the section where the symbol needing a cap_reloc is defined
    // TODO: just assume this is the way it has to be and error out otherwise to remove all the if statements
    if (DefinedRegular* DefinedLocation = dyn_cast<DefinedRegular>(LocationSym)) {
      if (InputSectionBase* IS = dyn_cast<InputSectionBase>(DefinedLocation->Section)) {
        if (DefinedLocation->isSection()) {
          // It seems like cap_relocs are generally .data(.rel.ro) + offset and not against the symbol itself
          // Try to convert it to a real symbol
          RealLocation = sectionWithOffsetToSymbol<ELFT>(IS, LocationOffset);
        } else {
          RealLocation = std::make_pair(DefinedLocation, 0);
        }
        SourceSection = IS;
        if (Config->VerboseCapRelocs)
          message("Adding capability relocation at " + toString(RealLocation.first ? *RealLocation.first : *LocationSym) +
                  " (" + DefinedLocation->Section->Name + "+0x" + utohexstr(LocationOffset) +
                  ")  against " + verboseToString<ELFT>(&TargetSym));
      } else {
        warn("Could not find InputSection for capability relocation at " +
             toString(*DefinedLocation) + "(" + DefinedLocation->Section->Name + "+0x" +
             utohexstr(LocationOffset) + ") against " + toString(TargetSym) + "\n");
      }
    } else {
      error("Unhandled symbol kind for cap_reloc: " + Twine(LocationSym->kind()));
      continue;
    }
    assert(RealLocation.first != nullptr);
    if (!SourceSection) {
      warn("Could not determine source section for cap_reloc used at " +
           S->template getObjMsg<ELFT>(CapRelocsOffset));
      SourceSection = S;
    }

    if (TargetSym.isUndefined()) {
      std::string Msg = "cap_reloc against undefined symbol: " + toString(TargetSym) +
                        "\n>>> referenced by " + verboseToString<ELFT>(RealLocation.first, RealLocation.second);
      if (Config->AllowUndefinedCapRelocs)
        warn(Msg);
      else
         error(Msg);
      continue;
    }
    bool TargetNeedsDynReloc = false;
    if (TargetSym.isPreemptible()) {
      // Do we need this?
      // TargetNeedsDynReloc = true;
    }
    switch (TargetSym.kind()) {
      case SymbolBody::DefinedRegularKind:
        break;
      case SymbolBody::DefinedCommonKind:
        // TODO: do I need to do anything special here?
        // message("Common symbol: " + toString(TargetSym));
        break;
      case SymbolBody::SharedKind:
        if (Config->Static) {
          error("cannot create a capability relocation against a shared symbol"
                        " when linking statically");
          continue;
        }
        // TODO: shouldn't undefined be an error?
        TargetNeedsDynReloc = true;
        break;
      default:
        error("Unhandled symbol kind for cap_reloc target: " +
              Twine(TargetSym.kind()));
        continue;
    }

    LocNeedsDynReloc = LocNeedsDynReloc || Config->Pic || Config->Pie;
    TargetNeedsDynReloc = TargetNeedsDynReloc || Config->Pic || Config->Pie;
    uint64_t CurrentEntryOffset = RelocsMap.size() * RelocSize;
    auto It = RelocsMap.insert(std::pair<CheriCapRelocLocation, CheriCapReloc>(
            {LocationSym, LocationOffset, LocNeedsDynReloc},
            {&TargetSym, TargetOffset, RawInput->offset, RawInput->size, TargetNeedsDynReloc}));
    if (!It.second) {
      // Maybe happens with vtables?
      error("Symbol already added to cap relocs");
      continue;
    }
    if (LocNeedsDynReloc) {
      assert(LocationSym->isSection());  // Needed because local symbols cannot be used in dynamic relocations
      // TODO: do this better
      // message("Adding dyn reloc at " + toString(this) + "+0x" + utohexstr(CurrentEntryOffset));
      assert(CurrentEntryOffset < getSize());
      // Add a dynamic relocation so that RTLD fills in the right base address
      // We only have the offset relative to the load address...
      // Ideally RTLD/crt_init_globals would just add the load address to all
      // cap_relocs entries that have a RELATIVE flag set instead of requiring a full
      // Elf_Rel/Elf_Rela
      // Can't use RealLocation here because that will usually refer to a local symbol
      In<ELFT>::RelaDyn->addReloc(
              {Target->RelativeRel, this, CurrentEntryOffset, true,
               LocationSym, static_cast<int64_t>(LocationOffset)});
    }
    if (TargetNeedsDynReloc) {
      // Capability target is the second field -> offset + 8
      uint64_t OffsetInOutSec = CurrentEntryOffset + 8;
      assert(OffsetInOutSec < getSize());
      // message("Adding dyn reloc at " + toString(this) + "+0x" + utohexstr(OffsetInOutSec) + " against " + toString(TargetSym));
      In<ELFT>::RelaDyn->addReloc(
              {Target->RelativeRel, this, OffsetInOutSec, false,
               &TargetSym, 0});  // Offset is always zero here because the capability offset is part of the __cap_reloc
    }
  }
}

template <class ELFT>
void CheriCapRelocsSection<ELFT>::writeTo(uint8_t *Buf) {
  constexpr endianness E = ELFT::TargetEndianness;
  static_assert(RelocSize == sizeof(InMemoryCapRelocEntry<E>), "cap relocs size mismatch");
  uint64_t Offset = 0;
  for (const auto &I : RelocsMap) {
    const CheriCapRelocLocation& Location = I.first;
    const CheriCapReloc& Reloc = I.second;
    SymbolBody *LocationSym = Location.BaseSym;
    int64_t LocationOffset = Location.Offset;
    // If we don't need a dynamic relocation just write the VA
    // We always write the virtual address here:
    // In the shared library case this will be an address relative to the load
    // address and will be handled by crt_init_globals. In the static case we
    // can compute the final virtual address
    uint64_t LocationVA = LocationSym->getVA(LocationOffset);
    // For the target the virtual address the addend is always zero so
    // if we need a dynamic reloc we write zero
    // TODO: would it be more efficient for local symbols to write the DSO VA
    // and add a relocation against the load address?
    // Also this would make llvm-objdump -C more useful because it would
    // actually display the symbol that the relocation is against
    uint64_t TargetVA = Reloc.Target->getVA(Reloc.TargetSymbolOffset);
    uint64_t TargetOffset = Reloc.Offset;
    uint64_t TargetSize = Reloc.Target->template getSize<ELFT>();
    if (TargetSize == 0) {
      warn("could not determine size of cap reloc " +
           getCapRelocSource<ELFT>(Location, Reloc));
      // TODO: check .size.foo symbols
      // TODO: also make this work for shared symbols
      if (OutputSection* OS = Reloc.Target->getOutputSection()) {
        TargetSize = OS->Size;
        // TODO: subtract offset of symbol in OS also for non-bss symbols
        if (auto* CSym = dyn_cast<DefinedCommon>(Reloc.Target)) {
          TargetSize -= CSym->Offset;
        }
        // FIXME: subtract OS->getOffset(Reloc.Target)
      } else {
        warn("Could not find size for symbol '" + toString(*Reloc.Target) +
             "' and could not determine section size. Using UINT64_MAX.");
        TargetSize = std::numeric_limits<uint64_t>::max();
      }
    }
    assert(TargetOffset <= TargetSize);
    uint64_t Permissions = Reloc.Target->isFunc() ? 1ULL << 63 : 0;
    InMemoryCapRelocEntry<E> Entry { LocationVA, TargetVA, TargetOffset, TargetSize, Permissions };
    memcpy(Buf + Offset, &Entry, sizeof(Entry));
//     if (Config->Verbose) {
//       errs() << "Added capability reloc: loc=" << utohexstr(LocationVA)
//              << ", object=" << utohexstr(TargetVA)
//              << ", offset=" << utohexstr(TargetOffset)
//              << ", size=" << utohexstr(TargetSize)
//              << ", permissions=" << utohexstr(Permissions) << "\n";
//     }
    Offset += RelocSize;
  }
  assert(Offset == getSize() && "Not all data written?");
}

InputSection *InX::ARMAttributes;
BssSection *InX::Bss;
BssSection *InX::BssRelRo;
BuildIdSection *InX::BuildId;
InputSection *InX::Common;
SyntheticSection *InX::Dynamic;
StringTableSection *InX::DynStrTab;
SymbolTableBaseSection *InX::DynSymTab;
InputSection *InX::Interp;
GdbIndexSection *InX::GdbIndex;
GotSection *InX::Got;
GotPltSection *InX::GotPlt;
GnuHashTableSection *InX::GnuHashTab;
IgotPltSection *InX::IgotPlt;
MipsGotSection *InX::MipsGot;
MipsRldMapSection *InX::MipsRldMap;
PltSection *InX::Plt;
PltSection *InX::Iplt;
StringTableSection *InX::ShStrTab;
StringTableSection *InX::StrTab;
SymbolTableBaseSection *InX::SymTab;

template void MipsGotSection::build<ELF32LE>();
template void MipsGotSection::build<ELF32BE>();
template void MipsGotSection::build<ELF64LE>();
template void MipsGotSection::build<ELF64BE>();

template void PltSection::addEntry<ELF32LE>(SymbolBody &Sym);
template void PltSection::addEntry<ELF32BE>(SymbolBody &Sym);
template void PltSection::addEntry<ELF64LE>(SymbolBody &Sym);
template void PltSection::addEntry<ELF64BE>(SymbolBody &Sym);

template InputSection *elf::createCommonSection<ELF32LE>();
template InputSection *elf::createCommonSection<ELF32BE>();
template InputSection *elf::createCommonSection<ELF64LE>();
template InputSection *elf::createCommonSection<ELF64BE>();

template MergeInputSection *elf::createCommentSection<ELF32LE>();
template MergeInputSection *elf::createCommentSection<ELF32BE>();
template MergeInputSection *elf::createCommentSection<ELF64LE>();
template MergeInputSection *elf::createCommentSection<ELF64BE>();

template class elf::MipsAbiFlagsSection<ELF32LE>;
template class elf::MipsAbiFlagsSection<ELF32BE>;
template class elf::MipsAbiFlagsSection<ELF64LE>;
template class elf::MipsAbiFlagsSection<ELF64BE>;

template class elf::MipsOptionsSection<ELF32LE>;
template class elf::MipsOptionsSection<ELF32BE>;
template class elf::MipsOptionsSection<ELF64LE>;
template class elf::MipsOptionsSection<ELF64BE>;

template class elf::MipsReginfoSection<ELF32LE>;
template class elf::MipsReginfoSection<ELF32BE>;
template class elf::MipsReginfoSection<ELF64LE>;
template class elf::MipsReginfoSection<ELF64BE>;

template class elf::DynamicSection<ELF32LE>;
template class elf::DynamicSection<ELF32BE>;
template class elf::DynamicSection<ELF64LE>;
template class elf::DynamicSection<ELF64BE>;

template class elf::RelocationSection<ELF32LE>;
template class elf::RelocationSection<ELF32BE>;
template class elf::RelocationSection<ELF64LE>;
template class elf::RelocationSection<ELF64BE>;

template class elf::SymbolTableSection<ELF32LE>;
template class elf::SymbolTableSection<ELF32BE>;
template class elf::SymbolTableSection<ELF64LE>;
template class elf::SymbolTableSection<ELF64BE>;

template class elf::HashTableSection<ELF32LE>;
template class elf::HashTableSection<ELF32BE>;
template class elf::HashTableSection<ELF64LE>;
template class elf::HashTableSection<ELF64BE>;

template class elf::EhFrameHeader<ELF32LE>;
template class elf::EhFrameHeader<ELF32BE>;
template class elf::EhFrameHeader<ELF64LE>;
template class elf::EhFrameHeader<ELF64BE>;

template class elf::VersionTableSection<ELF32LE>;
template class elf::VersionTableSection<ELF32BE>;
template class elf::VersionTableSection<ELF64LE>;
template class elf::VersionTableSection<ELF64BE>;

template class elf::VersionNeedSection<ELF32LE>;
template class elf::VersionNeedSection<ELF32BE>;
template class elf::VersionNeedSection<ELF64LE>;
template class elf::VersionNeedSection<ELF64BE>;

template class elf::VersionDefinitionSection<ELF32LE>;
template class elf::VersionDefinitionSection<ELF32BE>;
template class elf::VersionDefinitionSection<ELF64LE>;
template class elf::VersionDefinitionSection<ELF64BE>;

template class elf::EhFrameSection<ELF32LE>;
template class elf::EhFrameSection<ELF32BE>;
template class elf::EhFrameSection<ELF64LE>;
template class elf::EhFrameSection<ELF64BE>;

template class elf::CheriCapRelocsSection<ELF32LE>;
template class elf::CheriCapRelocsSection<ELF32BE>;
template class elf::CheriCapRelocsSection<ELF64LE>;
template class elf::CheriCapRelocsSection<ELF64BE>;
