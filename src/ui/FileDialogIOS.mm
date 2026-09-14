#include "ui/FileDialogIOS.hpp"

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <SDL3/SDL_video.h>

#include <filesystem>
#include <system_error>

namespace np {
namespace {

namespace fs = std::filesystem;

// The `UIWindow*` SDL created for `parentWindow`, or nil if there is none yet
// -- mirrors the cocoa backend's own `g_parentWindow == nullptr` fallback,
// just one property name later (`SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER`
// instead of the cocoa one), and the one thing that keeps this file from
// reaching into `[UIApplication sharedApplication]` globally, which the
// task's own brief calls out as the wrong way to find it.
UIWindow* uiWindowFor(SDL_Window* window) {
  if (window == nullptr) return nil;
  const SDL_PropertiesID props = SDL_GetWindowProperties(window);
  if (props == 0) return nil;
  void* ptr = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
  return (__bridge UIWindow*)ptr;
}

UIViewController* rootViewControllerFor(SDL_Window* window) {
  UIWindow* uiWindow = uiWindowFor(window);
  return uiWindow.rootViewController;
}

// Every extension in `rows`' patterns, turned into a `UTType`, nils dropped.
// Same rule the macOS cocoa backend measured and this header's comment
// quotes back: `abr`/`npaint`/`dpx` are not registered types but still
// resolve, to dynamic `dyn.*` identifiers that match by extension exactly
// like a registered one -- so this needs no hard-coded UTI table of its own.
NSArray<UTType*>* contentTypesFor(const std::vector<FileDialogFilterRow>& rows) {
  NSMutableArray<UTType*>* types = [NSMutableArray array];
  for (const FileDialogFilterRow& row : rows) {
    size_t start = 0;
    while (start <= row.pattern.size()) {
      const size_t sep = row.pattern.find(';', start);
      const std::string ext = row.pattern.substr(start, sep == std::string::npos ? sep : sep - start);
      if (!ext.empty()) {
        NSString* nsExt = [NSString stringWithUTF8String:ext.c_str()];
        UTType* type = [UTType typeWithFilenameExtension:nsExt];
        if (type != nil) [types addObject:type];
      }
      if (sep == std::string::npos) break;
      start = sep + 1;
    }
  }
  return types;
}

// One temp/import staging area per launch, under this app's own container --
// never `NSTemporaryDirectory()` directly, so a crash between the copy and
// the caller reading it does not leave a picked document sitting in a
// directory the OS can purge at any moment for any file.
fs::path importStagingDir() {
  NSArray<NSString*>* dirs =
      NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
  fs::path dir = fs::path([dirs.firstObject UTF8String]) / "Imported";
  std::error_code ec;
  fs::create_directories(dir, ec);
  return dir;
}

// Keeps the delegate alive for exactly one request, the same lifetime the
// mailbox's own "one request at a time" already enforces (ui/FileDialog.hpp).
// A picker's delegate is `weak` on the UIKit side, so nothing else holds this
// past the moment `dismissViewControllerAnimated:` drops the picker itself.
id<UIDocumentPickerDelegate> g_activeDelegate = nil;

}  // namespace
}  // namespace np

// The delegate. One class serves both the opening and exporting pickers --
// which mode ran is carried in `_exporting` because the two success paths
// post a different `FileDialogOutcome` shape (`alreadyWritten` and whether
// the URL needs copying in).
@interface NPFileDialogDelegate : NSObject <UIDocumentPickerDelegate>
- (instancetype)initWithMailbox:(np::FileDialogMailbox*)mailbox exporting:(BOOL)exporting;
@end

@implementation NPFileDialogDelegate {
  np::FileDialogMailbox* _mailbox;
  BOOL _exporting;
}

- (instancetype)initWithMailbox:(np::FileDialogMailbox*)mailbox exporting:(BOOL)exporting {
  if ((self = [super init])) {
    _mailbox = mailbox;
    _exporting = exporting;
  }
  return self;
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller
    didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  np::FileDialogOutcome outcome;
  NSURL* picked = urls.firstObject;
  if (picked == nil) {
    outcome.error = "The file panel closed with no file picked, and gave no reason.";
  } else if (_exporting) {
    // The picker has already copied the source file here. Nothing left to
    // write -- see ui/FileDialog.hpp's `alreadyWritten` field.
    outcome.chose = true;
    outcome.alreadyWritten = true;
    outcome.path = picked.path.UTF8String;
  } else {
    // Opening: `asCopy:YES` on the request below means this URL is already a
    // plain, non-security-scoped local file -- but it sits in a temp location
    // the picker itself owns and may reclaim once this method returns, so it
    // is copied again, into this app's own `Documents/Imported/`, before the
    // outcome is posted. Two copies for one open is the measured cost of
    // "every existing macOS-shaped caller keeps working unchanged" (this
    // header's own comment).
    namespace fs = std::filesystem;
    const fs::path src(picked.path.UTF8String);
    const fs::path dstDir = np::importStagingDir();
    const fs::path dst = dstDir / src.filename();
    std::error_code ec;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      outcome.error = "Could not copy the picked file into naturalPaint's own storage: " + ec.message();
    } else {
      outcome.chose = true;
      outcome.path = dst.string();
    }
  }
  _mailbox->post(std::move(outcome));
  np::g_activeDelegate = nil;
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
  np::FileDialogOutcome outcome;
  outcome.cancelled = true;
  _mailbox->post(std::move(outcome));
  np::g_activeDelegate = nil;
}

@end

namespace np {

bool showIOSOpenPicker(FileDialogPurpose /*purpose*/, const std::vector<FileDialogFilterRow>& rows,
                       SDL_Window* parentWindow, FileDialogMailbox* mailbox) {
  UIViewController* presenter = rootViewControllerFor(parentWindow);
  if (presenter == nil) return false;

  NSArray<UTType*>* types = contentTypesFor(rows);
  if (types.count == 0) types = @[ UTTypeItem ];  // Never an empty array -- see FileDialog.cpp's
                                                   // own comment on why that means "everything
                                                   // greyed out", not "no filter".

  NPFileDialogDelegate* delegate = [[NPFileDialogDelegate alloc] initWithMailbox:mailbox
                                                                       exporting:NO];
  g_activeDelegate = delegate;

  UIDocumentPickerViewController* picker =
      [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types asCopy:YES];
  picker.delegate = delegate;
  picker.allowsMultipleSelection = NO;

  dispatch_async(dispatch_get_main_queue(), ^{
    [presenter presentViewController:picker animated:YES completion:nil];
  });
  return true;
}

bool showIOSExportPicker(const std::string& sourcePath, SDL_Window* parentWindow,
                         FileDialogMailbox* mailbox) {
  UIViewController* presenter = rootViewControllerFor(parentWindow);
  if (presenter == nil) return false;

  NSURL* sourceURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:sourcePath.c_str()]];

  NPFileDialogDelegate* delegate = [[NPFileDialogDelegate alloc] initWithMailbox:mailbox
                                                                       exporting:YES];
  g_activeDelegate = delegate;

  UIDocumentPickerViewController* picker =
      [[UIDocumentPickerViewController alloc] initForExportingURLs:@[ sourceURL ] asCopy:YES];
  picker.delegate = delegate;

  dispatch_async(dispatch_get_main_queue(), ^{
    [presenter presentViewController:picker animated:YES completion:nil];
  });
  return true;
}

}  // namespace np
