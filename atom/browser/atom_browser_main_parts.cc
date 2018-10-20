// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/browser/atom_browser_main_parts.h"

#include <utility>

#if defined(OS_LINUX)
#include <glib.h>  // for g_setenv()
#endif

#include "atom/app/atom_main_delegate.h"
#include "atom/browser/api/atom_api_app.h"
#include "atom/browser/api/trackable_object.h"
#include "atom/browser/atom_browser_client.h"
#include "atom/browser/atom_browser_context.h"
#include "atom/browser/atom_web_ui_controller_factory.h"
#include "atom/browser/browser.h"
#include "atom/browser/io_thread.h"
#include "atom/browser/javascript_environment.h"
#include "atom/browser/media/media_capture_devices_dispatcher.h"
#include "atom/browser/node_debugger.h"
#include "atom/browser/ui/devtools_manager_delegate.h"
#include "atom/common/api/atom_bindings.h"
#include "atom/common/asar/asar_util.h"
#include "atom/common/node_bindings.h"
#include "base/base_switches.h"
#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/message_loop/message_loop.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "brightray/browser/brightray_paths.h"
#include "brightray/common/application_info.h"
#include "chrome/browser/browser_process_impl.h"
#include "chrome/browser/icon_manager.h"
#include "chrome/browser/net/chrome_net_log_helper.h"
#include "components/net_log/chrome_net_log.h"
#include "components/net_log/net_export_file_writer.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/child_process_security_policy.h"
#include "content/public/browser/web_ui_controller_factory.h"
#include "content/public/common/content_features.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/result_codes.h"
#include "content/public/common/service_manager_connection.h"
#include "electron/buildflags/buildflags.h"
#include "media/base/localized_strings.h"
#include "services/device/public/mojom/constants.mojom.h"
#include "services/network/public/cpp/network_switches.h"
#include "services/service_manager/public/cpp/connector.h"
#include "ui/base/idle/idle.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/material_design/material_design_controller.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/base/ui_base_switches.h"

#if defined(USE_AURA)
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/views/widget/desktop_aura/desktop_screen.h"
#include "ui/wm/core/wm_state.h"
#endif

#if defined(USE_X11)
#include "base/environment.h"
#include "base/nix/xdg_util.h"
#include "base/threading/thread_task_runner_handle.h"
#include "chrome/browser/ui/libgtkui/gtk_ui.h"
#include "chrome/browser/ui/libgtkui/gtk_util.h"
#include "ui/base/x/x11_util.h"
#include "ui/base/x/x11_util_internal.h"
#include "ui/events/devices/x11/touch_factory_x11.h"
#include "ui/views/linux_ui/linux_ui.h"
#endif

#if defined(OS_WIN)
#include "ui/base/cursor/cursor_loader_win.h"
#include "ui/base/l10n/l10n_util_win.h"
#include "ui/gfx/platform_font_win.h"
#endif

#if defined(OS_MACOSX)
#include "atom/browser/ui/cocoa/views_delegate_mac.h"
#else
#include "atom/browser/ui/views/atom_views_delegate.h"
#endif

#if defined(OS_LINUX)
#include "device/bluetooth/bluetooth_adapter_factory.h"
#include "device/bluetooth/dbus/dbus_bluez_manager_wrapper_linux.h"
#endif

// Must be included after all other headers.
#include "atom/common/node_includes.h"

namespace atom {

namespace {

template <typename T>
void Erase(T* container, typename T::iterator iter) {
  container->erase(iter);
}

#if defined(OS_WIN)
// gfx::Font callbacks
void AdjustUIFont(LOGFONT* logfont) {
  l10n_util::AdjustUIFont(logfont);
}

int GetMinimumFontSize() {
  return 10;
}
#endif

base::string16 MediaStringProvider(media::MessageId id) {
  switch (id) {
    case media::DEFAULT_AUDIO_DEVICE_NAME:
      return base::ASCIIToUTF16("Default");
#if defined(OS_WIN)
    case media::COMMUNICATIONS_AUDIO_DEVICE_NAME:
      return base::ASCIIToUTF16("Communications");
#endif
    default:
      return base::string16();
  }
}

#if defined(USE_X11)
// Indicates that we're currently responding to an IO error (by shutting down).
bool g_in_x11_io_error_handler = false;

// Number of seconds to wait for UI thread to get an IO error if we get it on
// the background thread.
const int kWaitForUIThreadSeconds = 10;

void OverrideLinuxAppDataPath() {
  base::FilePath path;
  if (base::PathService::Get(DIR_APP_DATA, &path))
    return;
  std::unique_ptr<base::Environment> env(base::Environment::Create());
  path = base::nix::GetXDGDirectory(env.get(), base::nix::kXdgConfigHomeEnvVar,
                                    base::nix::kDotConfigDir);
  base::PathService::Override(DIR_APP_DATA, path);
}

int BrowserX11ErrorHandler(Display* d, XErrorEvent* error) {
  if (!g_in_x11_io_error_handler && base::ThreadTaskRunnerHandle::IsSet()) {
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::Bind(&ui::LogErrorEventDescription, d, *error));
  }
  return 0;
}

// This function is used to help us diagnose crash dumps that happen
// during the shutdown process.
NOINLINE void WaitingForUIThreadToHandleIOError() {
  // Ensure function isn't optimized away.
  asm("");
  sleep(kWaitForUIThreadSeconds);
}

int BrowserX11IOErrorHandler(Display* d) {
  if (!content::BrowserThread::CurrentlyOn(content::BrowserThread::UI)) {
    // Wait for the UI thread (which has a different connection to the X server)
    // to get the error. We can't call shutdown from this thread without
    // tripping an error. Doing it through a function so that we'll be able
    // to see it in any crash dumps.
    WaitingForUIThreadToHandleIOError();
    return 0;
  }

  // If there's an IO error it likely means the X server has gone away.
  // If this DCHECK fails, then that means SessionEnding() below triggered some
  // code that tried to talk to the X server, resulting in yet another error.
  DCHECK(!g_in_x11_io_error_handler);

  g_in_x11_io_error_handler = true;
  LOG(ERROR) << "X IO error received (X server probably went away)";
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::RunLoop::QuitCurrentWhenIdleClosureDeprecated());

  return 0;
}

int X11EmptyErrorHandler(Display* d, XErrorEvent* error) {
  return 0;
}

int X11EmptyIOErrorHandler(Display* d) {
  return 0;
}
#endif

}  // namespace

void AtomBrowserMainParts::InitializeFeatureList() {
  auto* cmd_line = base::CommandLine::ForCurrentProcess();
  auto enable_features =
      cmd_line->GetSwitchValueASCII(::switches::kEnableFeatures);
  // Node depends on SharedArrayBuffer support, which was temporarily disabled
  // by https://chromium-review.googlesource.com/c/chromium/src/+/849429 (in
  // M64) and reenabled by
  // https://chromium-review.googlesource.com/c/chromium/src/+/1159358 (in
  // M70). Once Electron upgrades to M70, we can remove this.
  enable_features += std::string(",") + features::kSharedArrayBuffer.name;
  auto disable_features =
      cmd_line->GetSwitchValueASCII(::switches::kDisableFeatures);
#if defined(OS_MACOSX)
  // Disable the V2 sandbox on macOS.
  // Chromium is going to use the system sandbox API of macOS for the sandbox
  // implmentation, we may have to deprecate --mixed-sandbox for macOS once
  // Chromium drops support for the old sandbox implmentation.
  disable_features += std::string(",") + features::kMacV2Sandbox.name;
#endif
  auto feature_list = std::make_unique<base::FeatureList>();
  feature_list->InitializeFromCommandLine(enable_features, disable_features);
  base::FeatureList::SetInstance(std::move(feature_list));
}

#if !defined(OS_MACOSX)
void AtomBrowserMainParts::OverrideAppLogsPath() {
  base::FilePath path;
  if (base::PathService::Get(brightray::DIR_APP_DATA, &path)) {
    path = path.Append(base::FilePath::FromUTF8Unsafe(GetApplicationName()));
    path = path.Append(base::FilePath::FromUTF8Unsafe("logs"));
    base::PathService::Override(DIR_APP_LOGS, path);
  }
}
#endif

// static
AtomBrowserMainParts* AtomBrowserMainParts::self_ = nullptr;

AtomBrowserMainParts::AtomBrowserMainParts(
    const content::MainFunctionParams& params)
    : fake_browser_process_(new BrowserProcessImpl),
      browser_(new Browser),
      node_bindings_(NodeBindings::Create(NodeBindings::BROWSER)),
      atom_bindings_(new AtomBindings(uv_default_loop())),
      main_function_params_(params) {
  DCHECK(!self_) << "Cannot have two AtomBrowserMainParts";
  self_ = this;
  // Register extension scheme as web safe scheme.
  content::ChildProcessSecurityPolicy::GetInstance()->RegisterWebSafeScheme(
      "chrome-extension");
}

AtomBrowserMainParts::~AtomBrowserMainParts() {
  asar::ClearArchives();
  // Leak the JavascriptEnvironment on exit.
  // This is to work around the bug that V8 would be waiting for background
  // tasks to finish on exit, while somehow it waits forever in Electron, more
  // about this can be found at
  // https://github.com/electron/electron/issues/4767. On the other handle there
  // is actually no need to gracefully shutdown V8 on exit in the main process,
  // we already ensured all necessary resources get cleaned up, and it would
  // make quitting faster.
  ignore_result(js_env_.release());
}

// static
AtomBrowserMainParts* AtomBrowserMainParts::Get() {
  DCHECK(self_);
  return self_;
}

bool AtomBrowserMainParts::SetExitCode(int code) {
  if (!exit_code_)
    return false;

  *exit_code_ = code;
  return true;
}

int AtomBrowserMainParts::GetExitCode() {
  return exit_code_ != nullptr ? *exit_code_ : 0;
}

void AtomBrowserMainParts::RegisterDestructionCallback(
    base::OnceClosure callback) {
  // The destructors should be called in reversed order, so dependencies between
  // JavaScript objects can be correctly resolved.
  // For example WebContentsView => WebContents => Session.
  destructors_.insert(destructors_.begin(), std::move(callback));
}

bool AtomBrowserMainParts::ShouldContentCreateFeatureList() {
  return false;
}

int AtomBrowserMainParts::PreEarlyInitialization() {
  InitializeFeatureList();
  OverrideAppLogsPath();
#if defined(USE_X11)
  views::LinuxUI::SetInstance(BuildGtkUi());
  OverrideLinuxAppDataPath();

  // Installs the X11 error handlers for the browser process used during
  // startup. They simply print error messages and exit because
  // we can't shutdown properly while creating and initializing services.
  ui::SetX11ErrorHandlers(nullptr, nullptr);
#endif

#if defined(OS_POSIX)
  HandleSIGCHLD();
#endif

  return service_manager::RESULT_CODE_NORMAL_EXIT;
}

void AtomBrowserMainParts::PostEarlyInitialization() {
  // A workaround was previously needed because there was no ThreadTaskRunner
  // set.  If this check is failing we may need to re-add that workaround
  DCHECK(base::ThreadTaskRunnerHandle::IsSet());

  // The ProxyResolverV8 has setup a complete V8 environment, in order to
  // avoid conflicts we only initialize our V8 environment after that.
  js_env_.reset(new JavascriptEnvironment(node_bindings_->uv_loop()));

  node_bindings_->Initialize();
  // Create the global environment.
  node::Environment* env = node_bindings_->CreateEnvironment(
      js_env_->context(), js_env_->platform());
  node_env_.reset(new NodeEnvironment(env));

  // Enable support for v8 inspector
  node_debugger_.reset(new NodeDebugger(env));
  node_debugger_->Start();

  // Add Electron extended APIs.
  atom_bindings_->BindTo(js_env_->isolate(), env->process_object());

  // Load everything.
  node_bindings_->LoadEnvironment(env);

  // Wrap the uv loop with global env.
  node_bindings_->set_uv_env(env);

  // We already initialized the feature list in PreEarlyInitialization(), but
  // the user JS script would not have had a chance to alter the command-line
  // switches at that point. Lets reinitialize it here to pick up the
  // command-line changes.
  base::FeatureList::ClearInstanceForTesting();
  InitializeFeatureList();
}

int AtomBrowserMainParts::PreCreateThreads() {
#if defined(USE_AURA)
  display::Screen* screen = views::CreateDesktopScreen();
  display::Screen::SetScreenInstance(screen);
#if defined(USE_X11)
  views::LinuxUI::instance()->UpdateDeviceScaleFactor();
#endif
#endif

  if (!views::LayoutProvider::Get())
    layout_provider_.reset(new views::LayoutProvider());

  // Initialize the app locale.
  AtomBrowserClient::SetApplicationLocale(
      l10n_util::GetApplicationLocale(custom_locale_));

  fake_browser_process_->SetApplicationLocale(
      AtomBrowserClient::Get()->GetApplicationLocale());

  // Force MediaCaptureDevicesDispatcher to be created on UI thread.
  MediaCaptureDevicesDispatcher::GetInstance();

  // Force MediaCaptureDevicesDispatcher to be created on UI thread.
  MediaCaptureDevicesDispatcher::GetInstance();

#if defined(OS_MACOSX)
  ui::InitIdleMonitor();
#endif

  net_log_ = std::make_unique<net_log::ChromeNetLog>();
  auto& command_line = main_function_params_.command_line;
  // start net log trace if --log-net-log is passed in the command line.
  if (command_line.HasSwitch(network::switches::kLogNetLog)) {
    base::FilePath log_file =
        command_line.GetSwitchValuePath(network::switches::kLogNetLog);
    if (!log_file.empty()) {
      net_log_->StartWritingToFile(
          log_file, GetNetCaptureModeFromCommandLine(command_line),
          command_line.GetCommandLineString(), std::string());
    }
  }
  // Initialize net log file exporter.
  net_log_->net_export_file_writer()->Initialize();

  // Manage global state of net and other IO thread related.
  io_thread_ = std::make_unique<IOThread>(net_log_.get());

  return 0;
}

void AtomBrowserMainParts::PostDestroyThreads() {
#if defined(OS_LINUX)
  device::BluetoothAdapterFactory::Shutdown();
  bluez::DBusBluezManagerWrapperLinux::Shutdown();
#endif
  io_thread_.reset();
}

void AtomBrowserMainParts::ToolkitInitialized() {
  ui::MaterialDesignController::Initialize();

#if defined(USE_AURA) && defined(USE_X11)
  views::LinuxUI::instance()->Initialize();
#endif

#if defined(USE_AURA)
  wm_state_.reset(new wm::WMState);
#endif

#if defined(OS_WIN)
  gfx::PlatformFontWin::adjust_font_callback = &AdjustUIFont;
  gfx::PlatformFontWin::get_minimum_font_size_callback = &GetMinimumFontSize;

  wchar_t module_name[MAX_PATH] = {0};
  if (GetModuleFileName(NULL, module_name, MAX_PATH))
    ui::CursorLoaderWin::SetCursorResourceModule(module_name);
#endif

#if defined(OS_MACOSX)
  views_delegate_.reset(new ViewsDelegateMac);
#else
  views_delegate_.reset(new ViewsDelegate);
#endif
}

void AtomBrowserMainParts::PreMainMessageLoopRun() {
  // Run user's main script before most things get initialized, so we can have
  // a chance to setup everything.
  node_bindings_->PrepareMessageLoop();
  node_bindings_->RunMessageLoop();

#if defined(USE_X11)
  ui::TouchFactory::SetTouchDeviceListFromCommandLine();
#endif

  // Start idle gc.
  gc_timer_.Start(FROM_HERE, base::TimeDelta::FromMinutes(1),
                  base::Bind(&v8::Isolate::LowMemoryNotification,
                             base::Unretained(js_env_->isolate())));

  content::WebUIControllerFactory::RegisterFactory(
      AtomWebUIControllerFactory::GetInstance());

  // --remote-debugging-port
  auto* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(switches::kRemoteDebuggingPort))
    DevToolsManagerDelegate::StartHttpHandler();

#if defined(USE_X11)
  libgtkui::GtkInitFromCommandLine(*base::CommandLine::ForCurrentProcess());
#endif

#if !defined(OS_MACOSX)
  // The corresponding call in macOS is in AtomApplicationDelegate.
  Browser::Get()->WillFinishLaunching();
  Browser::Get()->DidFinishLaunching(base::DictionaryValue());
#endif

  // Notify observers that main thread message loop was initialized.
  Browser::Get()->PreMainMessageLoopRun();
}

bool AtomBrowserMainParts::MainMessageLoopRun(int* result_code) {
  js_env_->OnMessageLoopCreated();
  exit_code_ = result_code;
  return content::BrowserMainParts::MainMessageLoopRun(result_code);
}

void AtomBrowserMainParts::PreDefaultMainMessageLoopRun(
    base::OnceClosure quit_closure) {
  Browser::SetMainMessageLoopQuitClosure(std::move(quit_closure));
}

void AtomBrowserMainParts::PostMainMessageLoopStart() {
#if defined(USE_X11)
  // Installs the X11 error handlers for the browser process after the
  // main message loop has started. This will allow us to exit cleanly
  // if X exits before us.
  ui::SetX11ErrorHandlers(BrowserX11ErrorHandler, BrowserX11IOErrorHandler);
#endif
#if defined(OS_LINUX)
  bluez::DBusBluezManagerWrapperLinux::Initialize();
#endif
#if defined(OS_POSIX)
  HandleShutdownSignals();
#endif
}

void AtomBrowserMainParts::PostMainMessageLoopRun() {
#if defined(USE_X11)
  // Unset the X11 error handlers. The X11 error handlers log the errors using a
  // |PostTask()| on the message-loop. But since the message-loop is in the
  // process of terminating, this can cause errors.
  ui::SetX11ErrorHandlers(X11EmptyErrorHandler, X11EmptyIOErrorHandler);
#endif

  js_env_->OnMessageLoopDestroying();

#if defined(OS_MACOSX)
  FreeAppDelegate();
#endif

  // Make sure destruction callbacks are called before message loop is
  // destroyed, otherwise some objects that need to be deleted on IO thread
  // won't be freed.
  // We don't use ranged for loop because iterators are getting invalided when
  // the callback runs.
  for (auto iter = destructors_.begin(); iter != destructors_.end();) {
    base::OnceClosure callback = std::move(*iter);
    if (!callback.is_null())
      std::move(callback).Run();
    ++iter;
  }
}

#if !defined(OS_MACOSX)
void AtomBrowserMainParts::PreMainMessageLoopStart() {
  PreMainMessageLoopStartCommon();
}
#endif

void AtomBrowserMainParts::PreMainMessageLoopStartCommon() {
  // Initialize ui::ResourceBundle.
  ui::ResourceBundle::InitSharedInstanceWithLocale(
      "", nullptr, ui::ResourceBundle::DO_NOT_LOAD_COMMON_RESOURCES);
  auto* cmd_line = base::CommandLine::ForCurrentProcess();
  if (cmd_line->HasSwitch(switches::kLang)) {
    const std::string locale = cmd_line->GetSwitchValueASCII(switches::kLang);
    const base::FilePath locale_file_path =
        ui::ResourceBundle::GetSharedInstance().GetLocaleFilePath(locale, true);
    if (!locale_file_path.empty()) {
      custom_locale_ = locale;
#if defined(OS_LINUX)
      /* When built with USE_GLIB, libcc's GetApplicationLocaleInternal() uses
       * glib's g_get_language_names(), which keys off of getenv("LC_ALL") */
      g_setenv("LC_ALL", custom_locale_.c_str(), TRUE);
#endif
    }
  }

#if defined(OS_MACOSX)
  if (custom_locale_.empty())
    l10n_util::OverrideLocaleWithCocoaLocale();
#endif
  LoadResourceBundle(custom_locale_);
#if defined(OS_MACOSX)
  InitializeMainNib();
#endif
  media::SetLocalizedStringProvider(MediaStringProvider);
}

device::mojom::GeolocationControl*
AtomBrowserMainParts::GetGeolocationControl() {
  if (geolocation_control_)
    return geolocation_control_.get();

  auto request = mojo::MakeRequest(&geolocation_control_);
  if (!content::ServiceManagerConnection::GetForProcess())
    return geolocation_control_.get();

  service_manager::Connector* connector =
      content::ServiceManagerConnection::GetForProcess()->GetConnector();
  connector->BindInterface(device::mojom::kServiceName, std::move(request));
  return geolocation_control_.get();
}

IconManager* AtomBrowserMainParts::GetIconManager() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!icon_manager_.get())
    icon_manager_.reset(new IconManager);
  return icon_manager_.get();
}

}  // namespace atom
