#include "clang/AST/ModLoaderSpaces.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/DiagnosticAST.h"
#include "clang/Basic/TargetInfo.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/MathExtras.h"
#include <memory>

using namespace clang;
using namespace clang::modloader;

const Space *SpaceTable::lookup(llvm::StringRef Name) const {
  for (const Space &S : Spaces)
    if (S.Name == Name)
      return &S;

  return nullptr;
}

const Space *SpaceTable::lookup(unsigned Id) const {
  for (const Space &S : Spaces)
    if (S.Id == Id)
      return &S;

  return nullptr;
}

std::string SpaceTable::add(const Space &NewSpace) {
  if (NewSpace.Id > MaxGuestSpaceId)
    return "space id exceeds Clang's address-space representation";

  if (NewSpace.Name == HostSpaceName)
    return "'host' is the module's own memory and cannot be declared";

  if (NewSpace.Width != 32 && NewSpace.Width != 64)
    return "a space is 32 or 64 bits wide";

  if (NewSpace.Mask > llvm::maskTrailingOnes<uint64_t>(NewSpace.Width))
    return "the mask is wider than the space";

  if (NewSpace.Id == FlatSpaceId && NewSpace.Kind != Codec::Flat)
    return "space 0 is the flat CPU space and takes the codec 'flat'";

  if (NewSpace.Id != FlatSpaceId && NewSpace.Kind == Codec::Flat)
    return "only space 0 can be flat";

  if (NewSpace.TrackDirty && NewSpace.Mask > 0xFFFFFFFFULL)
    return "a space that tracks dirty pages needs a mask below 4 GiB";

  for (const Space &S : Spaces) {
    bool SameName = S.Name == NewSpace.Name;
    bool SameId = S.Id == NewSpace.Id;
    if (SameName && SameId && S.Kind == NewSpace.Kind &&
        S.Width == NewSpace.Width && S.Mask == NewSpace.Mask &&
        S.TrackDirty == NewSpace.TrackDirty && S.ABI == NewSpace.ABI)
      return "";

    if (SameName || SameId)
      return "conflicts with the earlier declaration of space '" + S.Name + "'";
  }

  Spaces.push_back(NewSpace);
  return "";
}

std::string SpaceTable::addRegion(const Region &NewRegion) {
  const Space *Flat = lookup(NewRegion.FlatId);
  const Space *Target = lookup(NewRegion.TargetId);
  if (!Flat || Flat->Kind != Codec::Flat)
    return "a region starts from the flat space";

  if (!Target || Target->Kind == Codec::Flat)
    return "a region maps to a declared space other than the flat one";

  if (NewRegion.From >= NewRegion.To ||
      NewRegion.To - 1 > llvm::maskTrailingOnes<uint64_t>(Flat->Width))
    return Flat->Width >= 64
               ? "a region needs from < to"
               : "a region needs from < to <= 0x100000000 (the flat space is "
                 "32-bit)";

  for (const Region &R : Regions) {
    if (R.FlatId == NewRegion.FlatId && R.TargetId == NewRegion.TargetId &&
        R.From == NewRegion.From && R.To == NewRegion.To)
      return "";

    if (R.FlatId == NewRegion.FlatId && NewRegion.From < R.To &&
        R.From < NewRegion.To)
      return "overlaps an earlier region";
  }

  Regions.push_back(NewRegion);
  return "";
}

unsigned SpaceTable::getTargetAddressSpace(unsigned Id) const {
  const Space *S = lookup(Id);
  unsigned Width = S ? S->Width : 32;
  return llvm::ModLoader::getGuestAddressSpace(Id, Width);
}

LangAS SpaceTable::getAddressSpace(unsigned Id) const {
  return getLangASFromTargetAS(getTargetAddressSpace(Id));
}

GuestABI SpaceTable::getABI(const Space &S) const {
  if (S.ABI != GuestABI::None)
    return S.ABI;

  const Space *Flat = lookup(FlatSpaceId);
  return Flat ? Flat->ABI : GuestABI::None;
}

void SpaceTable::useSpace(unsigned File, const Defaults &NewDefaults) {
  if (!DefaultsStack.empty() && DefaultsStack.back().File == File) {
    DefaultsStack.back().Value = NewDefaults;
    return;
  }

  DefaultsStack.push_back({File, NewDefaults});
}

void SpaceTable::leaveFile(unsigned File) {
  if (File == 0)
    return;

  llvm::erase_if(DefaultsStack, [File](const DefaultsEntry &Entry) {
    return Entry.File == File;
  });
}

SpaceTable &ASTContext::getModLoaderSpaces() const {
  if (!ModLoaderSpaces)
    ModLoaderSpaces = std::make_unique<SpaceTable>(
        getTargetInfo().getTriple().getEnvironmentName() == "guest64" ? 64
                                                                      : 32);

  return *ModLoaderSpaces;
}

bool modloader::isGuestAddressSpace(LangAS AS) {
  return isTargetAddressSpace(AS) &&
         llvm::ModLoader::isGuestAddressSpace(toTargetAddressSpace(AS));
}

unsigned modloader::getGuestSpaceId(LangAS AS) {
  return llvm::ModLoader::getGuestSpaceId(toTargetAddressSpace(AS));
}

bool modloader::isHostOnlyType(QualType T) {
  const Type *Element = T->getBaseElementTypeUnsafe();
  return Element->isReferenceType() || Element->isMemberPointerType() ||
         Element->isBlockPointerType() ||
         (Element->isPointerType() &&
          !isGuestAddressSpace(Element->getPointeeType().getAddressSpace()));
}

bool modloader::inGuestABIRegion(const ASTContext &Context) {
  return Context.getModLoaderSpaces().getDefaults().ABI != GuestABI::None;
}

std::optional<LangAS>
modloader::getDefaultAddressSpace(const ASTContext &Context) {
  const SpaceTable &Table = Context.getModLoaderSpaces();
  Defaults Current = Table.getDefaults();
  if (!Current.Guest)
    return std::nullopt;

  return Table.getAddressSpace(Current.SpaceId);
}

static std::string toHex(CharUnits Value) {
  return "0x" + llvm::utohexstr(Value.getQuantity());
}

std::optional<CharUnits> modloader::getFieldOffset(const ASTContext &Context,
                                                   const FieldDecl *Field,
                                                   CharUnits DataSize) {
  const auto *Attr = Field->getAttr<ModLoaderFieldOffsetAttr>();
  if (!Attr)
    return std::nullopt;

  CharUnits Offset = CharUnits::fromQuantity(Attr->getOffset());
  if (Offset >= DataSize)
    return Offset;

  Context.getDiagnostics().Report(Attr->getLocation(),
                                  diag::err_modloader_field_offset_overlap)
      << Field << toHex(Offset) << toHex(DataSize);
  return std::nullopt;
}

CharUnits modloader::getStructSize(const ASTContext &Context,
                                   const NamedDecl *D, CharUnits Size,
                                   CharUnits DataSize, CharUnits Alignment) {
  const auto *Attr = D->getAttr<ModLoaderStructSizeAttr>();
  if (!Attr)
    return Size;

  CharUnits Requested = CharUnits::fromQuantity(Attr->getSize());
  if (Requested < DataSize)
    Context.getDiagnostics().Report(Attr->getLocation(),
                                    diag::err_modloader_struct_size_too_small)
        << D << toHex(Requested) << toHex(DataSize);
  else if (!Requested.isMultipleOf(Alignment))
    Context.getDiagnostics().Report(Attr->getLocation(),
                                    diag::err_modloader_struct_size_alignment)
        << D << toHex(Requested) << toHex(Alignment);
  else
    return Requested;

  return Size;
}

QualType modloader::withDefaultSpace(const ASTContext &Context, QualType T) {
  if (T.hasAddressSpace() || T->isDependentType() ||
      T->hasAttr(attr::ModLoaderHostSpace))
    return T;

  if (std::optional<LangAS> AS = getDefaultAddressSpace(Context))
    return Context.getAddrSpaceQualType(T, *AS);

  return T;
}
