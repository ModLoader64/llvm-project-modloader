#ifndef LLVM_CLANG_AST_MODLOADERSPACES_H
#define LLVM_CLANG_AST_MODLOADERSPACES_H

#include "clang/AST/CharUnits.h"
#include "clang/AST/Type.h"
#include "clang/Basic/AddressSpaces.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/TargetParser/ModLoaderAddressSpaces.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clang {
class ASTContext;
class FieldDecl;
class NamedDecl;

namespace modloader {

constexpr unsigned MaxGuestSpaceId =
    (Qualifiers::MaxAddressSpace - unsigned(LangAS::FirstTargetAddressSpace) -
     llvm::ModLoader::FirstGuestAddressSpace - 1) /
    2;
constexpr unsigned FlatSpaceId = 0;
constexpr llvm::StringLiteral HostSpaceName = "host";

using llvm::ModLoader::Codec;

enum class GuestABI { None, N64 };

struct Space {
  std::string Name;
  unsigned Id = 0;
  Codec Kind = Codec::LittleEndian;
  unsigned Width = 32; // Address bits: 32 or 64
  uint64_t Mask = 0xFFFFFFFF;
  bool TrackDirty = false;
  GuestABI ABI = GuestABI::None; // None: the flat space's
};

struct Region {
  unsigned FlatId = FlatSpaceId;
  unsigned TargetId = 0;
  uint64_t From = 0;
  uint64_t To = 0;
};

struct Defaults {
  bool Guest = false;
  unsigned SpaceId = FlatSpaceId;
  GuestABI ABI = GuestABI::None;
};

class SpaceTable {
public:
  explicit SpaceTable(unsigned DefaultWidth) : DefaultWidth(DefaultWidth) {}

  unsigned getDefaultWidth() const { return DefaultWidth; }

  const Space *lookup(llvm::StringRef Name) const;
  const Space *lookup(unsigned Id) const;

  std::string add(const Space &NewSpace);
  std::string addRegion(const Region &NewRegion);

  const std::vector<Space> &spaces() const { return Spaces; }
  const std::vector<Region> &regions() const { return Regions; }

  unsigned getTargetAddressSpace(unsigned Id) const;
  LangAS getAddressSpace(unsigned Id) const;
  GuestABI getABI(const Space &S) const;

  void useSpace(unsigned File, const Defaults &NewDefaults);
  void leaveFile(unsigned File);

  Defaults getDefaults() const {
    return DefaultsStack.empty() ? Defaults() : DefaultsStack.back().Value;
  }

private:
  struct DefaultsEntry {
    unsigned File = 0;
    Defaults Value;
  };

  unsigned DefaultWidth;
  std::vector<Space> Spaces;
  std::vector<Region> Regions;
  std::vector<DefaultsEntry> DefaultsStack;
};

bool isGuestAddressSpace(LangAS AS);
unsigned getGuestSpaceId(LangAS AS);
bool isHostOnlyType(QualType T);

bool inGuestABIRegion(const ASTContext &Context);
std::optional<LangAS> getDefaultAddressSpace(const ASTContext &Context);
/// Explicit address spaces and space<host> override use_space defaults
QualType withDefaultSpace(const ASTContext &Context, QualType T);

std::optional<CharUnits> getFieldOffset(const ASTContext &Context,
                                        const FieldDecl *Field,
                                        CharUnits DataSize);
CharUnits getStructSize(const ASTContext &Context, const NamedDecl *D,
                        CharUnits Size, CharUnits DataSize,
                        CharUnits Alignment);

} // namespace modloader
} // namespace clang

#endif // LLVM_CLANG_AST_MODLOADERSPACES_H
