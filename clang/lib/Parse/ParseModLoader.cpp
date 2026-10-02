#include "clang/AST/ModLoaderSpaces.h"
#include "clang/Basic/DiagnosticParse.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Parse/Parser.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/MathExtras.h"

using namespace clang;

bool Parser::isModLoaderSpaceQualifier() {
  if (!getLangOpts().ModLoader || Tok.isNot(tok::identifier) ||
      !Tok.getIdentifierInfo()->isStr("space") ||
      NextToken().isNot(tok::less) ||
      GetLookAheadToken(2).isNot(tok::identifier) ||
      GetLookAheadToken(3).isNot(tok::greater))
    return false;

  LookupResult Result(Actions, Tok.getIdentifierInfo(), Tok.getLocation(),
                      Sema::LookupOrdinaryName);
  return !Actions.LookupName(Result, getCurScope());
}

void Parser::ParseModLoaderSpaceQualifier(ParsedAttributes &Attrs) {
  assert(isModLoaderSpaceQualifier());
  SourceLocation SpaceLoc = ConsumeToken(); // 'space'
  ConsumeToken();                           // '<'
  IdentifierInfo *Name = Tok.getIdentifierInfo();
  SourceLocation NameLoc = ConsumeToken();
  SourceLocation EndLoc = ConsumeToken(); // '>'

  if (Name->getName() == modloader::HostSpaceName) {
    Attrs.addNew(PP.getIdentifierInfo("modloader_host_space"),
                 SourceRange(SpaceLoc, EndLoc), AttributeScopeInfo(), nullptr,
                 0, ParsedAttr::Form::GNU());
    return;
  }

  const modloader::SpaceTable &Table =
      Actions.getASTContext().getModLoaderSpaces();
  const modloader::Space *Space = Table.lookup(Name->getName());
  if (!Space) {
    Diag(NameLoc, diag::err_modloader_unknown_space) << Name;
    return;
  }

  ExprResult Value = Actions.ActOnIntegerConstant(
      SpaceLoc, Table.getTargetAddressSpace(Space->Id));
  ArgsUnion Argument(Value.get());
  Attrs.addNew(PP.getIdentifierInfo("address_space"),
               SourceRange(SpaceLoc, EndLoc), AttributeScopeInfo(), &Argument,
               1, ParsedAttr::Form::GNU());
}

void Parser::ConsumeModLoaderQualifier() {
  if (isModLoaderSpaceQualifier()) {
    ConsumeToken();
    ConsumeToken();
    ConsumeToken();
  }

  ConsumeToken();
}

namespace {
struct PragmaModLoaderHandler : public PragmaHandler {
  explicit PragmaModLoaderHandler(Sema &Actions)
      : PragmaHandler("modloader"), Actions(Actions),
        Live(std::make_shared<bool>(true)) {}

  ~PragmaModLoaderHandler() override { *Live = false; }

  void HandlePragma(Preprocessor &PP, PragmaIntroducer Introducer,
                    Token &FirstToken) override;

  Sema &Actions;
  std::shared_ptr<bool> Live; // false once the handler is gone
};

struct ModLoaderFileCallbacks : public PPCallbacks {
  ModLoaderFileCallbacks(const ASTContext &Context, std::shared_ptr<bool> Live)
      : Context(Context), Live(std::move(Live)) {}

  void FileChanged(SourceLocation Loc, FileChangeReason Reason,
                   SrcMgr::CharacteristicKind FileType,
                   FileID PrevFID) override {
    if (Reason == ExitFile && *Live && PrevFID.isValid())
      Context.getModLoaderSpaces().leaveFile(PrevFID.getHashValue());
  }

  const ASTContext &Context;
  std::shared_ptr<bool> Live;
};
} // namespace

std::unique_ptr<PragmaHandler> Parser::createModLoaderPragmaHandler() {
  auto Handler = std::make_unique<PragmaModLoaderHandler>(Actions);
  PP.addPPCallbacks(std::make_unique<ModLoaderFileCallbacks>(
      Actions.getASTContext(), Handler->Live));
  return Handler;
}

void PragmaModLoaderHandler::HandlePragma(Preprocessor &PP,
                                          PragmaIntroducer Introducer,
                                          Token &FirstToken) {
  auto Fail = [&](SourceLocation Loc, StringRef Message) {
    PP.Diag(Loc, diag::err_modloader_pragma) << Message;
  };

  Token Tok;
  PP.Lex(Tok);
  StringRef Directive = Tok.is(tok::identifier)
                            ? Tok.getIdentifierInfo()->getName()
                            : StringRef();
  if (Directive != "space" && Directive != "region" &&
      Directive != "use_space") {
    Fail(Tok.getLocation(), "expected 'space', 'region' or 'use_space'");
    return;
  }

  SourceLocation StartLoc = Tok.getLocation();
  PP.Lex(Tok);
  if (Tok.isNot(tok::l_paren)) {
    Fail(Tok.getLocation(), "expected '('");
    return;
  }

  struct Argument {
    SourceLocation Loc;
    IdentifierInfo *Name = nullptr;
    uint64_t Value = 0;
    bool IsOption = false;
    IdentifierInfo *OptionName = nullptr;
  };
  SmallVector<Argument, 8> Arguments;
  PP.Lex(Tok);
  while (true) {
    Argument Arg;
    Arg.Loc = Tok.getLocation();
    if (Tok.is(tok::identifier)) {
      Arg.Name = Tok.getIdentifierInfo();
      PP.Lex(Tok);
      if (Tok.is(tok::equal)) {
        Arg.IsOption = true;
        PP.Lex(Tok);
        if (Tok.is(tok::identifier)) {
          Arg.OptionName = Tok.getIdentifierInfo();
          PP.Lex(Tok);
        } else if (Tok.is(tok::numeric_constant)) {
          if (!PP.parseSimpleIntegerLiteral(Tok, Arg.Value)) {
            Fail(Arg.Loc, "expected an integer");
            return;
          }
        } else {
          Fail(Tok.getLocation(), "expected a name or an integer after '='");
          return;
        }
      }
    } else if (Tok.is(tok::numeric_constant)) {
      if (!PP.parseSimpleIntegerLiteral(Tok, Arg.Value)) {
        Fail(Arg.Loc, "expected an integer");
        return;
      }
    } else {
      Fail(Tok.getLocation(), "expected a name or an integer");
      return;
    }

    Arguments.push_back(Arg);
    if (Tok.is(tok::comma)) {
      PP.Lex(Tok);
      continue;
    }

    if (Tok.is(tok::r_paren)) {
      PP.Lex(Tok);
      break;
    }

    Fail(Tok.getLocation(), "expected ',' or ')'");
    return;
  }

  if (Tok.isNot(tok::eod)) {
    Fail(Tok.getLocation(), "unexpected tokens after ')'");
    return;
  }

  modloader::SpaceTable &Table = Actions.getASTContext().getModLoaderSpaces();
  std::string Error;
  if (Directive == "space") {
    if (Arguments.size() < 3 || !Arguments[0].Name || Arguments[0].IsOption ||
        Arguments[1].Name || !Arguments[2].Name || Arguments[2].IsOption) {
      Fail(StartLoc, "expected space(name, id, codec [, mask] [, dirty] "
                     "[, width=bits] [, abi=name])");
      return;
    }

    modloader::Space NewSpace;
    NewSpace.Width = Table.getDefaultWidth();
    NewSpace.Name = Arguments[0].Name->getName().str();
    if (Arguments[1].Value > modloader::MaxGuestSpaceId) {
      Fail(Arguments[1].Loc,
           "space id exceeds Clang's address-space representation");
      return;
    }

    NewSpace.Id = unsigned(Arguments[1].Value);
    auto Codec = llvm::ModLoader::parseCodec(Arguments[2].Name->getName());
    if (!Codec) {
      Fail(Arguments[2].Loc,
           "the codec is flat, little_endian, big_endian or word_swapped");
      return;
    }

    NewSpace.Kind = *Codec;
    std::optional<uint64_t> Mask;
    for (const Argument &Arg : ArrayRef<Argument>(Arguments).drop_front(3)) {
      if (Arg.IsOption && Arg.Name->isStr("width")) {
        if (Arg.OptionName || (Arg.Value != 32 && Arg.Value != 64)) {
          Fail(Arg.Loc, "width is 32 or 64");
          return;
        }

        NewSpace.Width = unsigned(Arg.Value);
      } else if (Arg.IsOption && Arg.Name->isStr("abi")) {
        if (!Arg.OptionName || !Arg.OptionName->isStr("n64")) {
          Fail(Arg.Loc, "the guest ABI is 'n64'");
          return;
        }

        NewSpace.ABI = modloader::GuestABI::N64;
      } else if (Arg.IsOption) {
        Fail(Arg.Loc, "the options are width=bits and abi=name");
        return;
      } else if (!Arg.Name) {
        Mask = Arg.Value;
      } else if (Arg.Name->isStr("dirty")) {
        NewSpace.TrackDirty = true;
      } else {
        Fail(Arg.Loc, "expected a mask, 'dirty', width=bits or abi=name");
        return;
      }
    }

    NewSpace.Mask =
        Mask.value_or(llvm::maskTrailingOnes<uint64_t>(NewSpace.Width));
    if (NewSpace.ABI == modloader::GuestABI::N64 && NewSpace.Width != 32) {
      Fail(StartLoc, "the n64 guest ABI has 32-bit pointers");
      return;
    }

    Error = Table.add(NewSpace);
  } else if (Directive == "region") {
    if (Arguments.size() != 4 || !Arguments[0].Name || !Arguments[1].Name ||
        Arguments[0].IsOption || Arguments[1].IsOption || Arguments[2].Name ||
        Arguments[3].Name) {
      Fail(StartLoc, "expected region(flat, space, from, to)");
      return;
    }

    const modloader::Space *Flat = Table.lookup(Arguments[0].Name->getName());
    const modloader::Space *Target = Table.lookup(Arguments[1].Name->getName());
    if (!Flat || !Target) {
      Fail(StartLoc, "a region names two declared spaces");
      return;
    }

    Error = Table.addRegion(
        {Flat->Id, Target->Id, Arguments[2].Value, Arguments[3].Value});
  } else {
    if (Arguments.size() != 1 || !Arguments[0].Name || Arguments[0].IsOption) {
      Fail(StartLoc, "expected use_space(name)");
      return;
    }

    const Argument &Arg = Arguments[0];
    modloader::Defaults NewDefaults;
    if (Arg.Name->getName() != modloader::HostSpaceName) {
      const modloader::Space *Space = Table.lookup(Arg.Name->getName());
      if (!Space) {
        Fail(Arg.Loc, "unknown guest space; declare it with '#pragma modloader "
                      "space' (include <modloader/platform.h>)");
        return;
      }

      NewDefaults.Guest = true;
      NewDefaults.SpaceId = Space->Id;
      NewDefaults.ABI = Table.getABI(*Space);
      if (NewDefaults.ABI == modloader::GuestABI::N64 && Space->Width != 32) {
        Fail(Arg.Loc, "the n64 guest ABI has 32-bit pointers");
        return;
      }
    }

    SourceManager &SM = PP.getSourceManager();
    FileID File = SM.getFileID(SM.getExpansionLoc(StartLoc));
    Table.useSpace(File.getHashValue(), NewDefaults);
  }

  if (!Error.empty())
    Fail(StartLoc, Error);
}
