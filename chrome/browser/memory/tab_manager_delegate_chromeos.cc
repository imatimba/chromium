// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/memory/tab_manager_delegate_chromeos.h"

#include <stdint.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "ash/shell.h"
#include "base/bind.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/memory/memory_pressure_monitor_chromeos.h"
#include "base/metrics/histogram_macros.h"
#include "base/process/process_handle.h"  // kNullProcessHandle.
#include "base/process/process_metrics.h"
#include "base/strings/string16.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "chrome/browser/chromeos/arc/process/arc_process.h"
#include "chrome/browser/chromeos/arc/process/arc_process_service.h"
#include "chrome/browser/memory/tab_stats.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/chrome_features.h"
#include "chromeos/dbus/dbus_thread_manager.h"
#include "components/arc/arc_bridge_service.h"
#include "components/arc/common/process.mojom.h"
#include "components/arc/metrics/oom_kills_histogram.h"
#include "components/exo/shell_surface.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/notification_service.h"
#include "content/public/browser/notification_types.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/zygote_host_linux.h"
#include "ui/aura/window.h"
#include "ui/wm/public/activation_client.h"

using base::ProcessHandle;
using base::TimeDelta;
using content::BrowserThread;

namespace memory {
namespace {

const char kExoShellSurfaceWindowName[] = "ExoShellSurface";
const char kArcProcessNamePrefix[] = "org.chromium.arc.";

// When switching to a new tab the tab's renderer's OOM score needs to be
// updated to reflect its front-most status and protect it from discard.
// However, doing this immediately might slow down tab switch time, so wait
// a little while before doing the adjustment.
const int kFocusedProcessScoreAdjustIntervalMs = 500;

const uint32_t kMinVersionForKillProcess = 1;

aura::client::ActivationClient* GetActivationClient() {
  if (!ash::Shell::HasInstance())
    return nullptr;
  return aura::client::GetActivationClient(ash::Shell::GetPrimaryRootWindow());
}

// Checks if a window renders ARC apps.
bool IsArcWindow(aura::Window* window) {
  if (!window || window->name() != kExoShellSurfaceWindowName)
    return false;
  std::string application_id = exo::ShellSurface::GetApplicationId(window);
  return base::StartsWith(application_id, kArcProcessNamePrefix,
                          base::CompareCase::SENSITIVE);
}

bool IsArcMemoryManagementEnabled() {
  return base::FeatureList::IsEnabled(features::kArcMemoryManagement);
}

void OnSetOomScoreAdj(bool success, const std::string& output) {
  VLOG(2) << "OnSetOomScoreAdj " << success << " " << output;
  if (!success || output != "")
    LOG(WARNING) << "Set OOM score error: " << output;
}

}  // namespace

std::ostream& operator<<(std::ostream& os, const ProcessType& type) {
  switch (type) {
    case ProcessType::FOCUSED_APP:
      return os << "FOCUSED_APP/FOCUSED_TAB";
    case ProcessType::VISIBLE_APP:
      return os << "VISIBLE_APP";
    case ProcessType::BACKGROUND_APP:
      return os << "BACKGROUND_APP";
    case ProcessType::BACKGROUND_TAB:
      return os << "BACKGROUND_TAB";
    case ProcessType::UNKNOWN_TYPE:
      return os << "UNKNOWN_TYPE";
    default:
      return os << "NOT_IMPLEMENTED_ERROR";
  }
  return os;
}

// TabManagerDelegate::Candidate implementation.
std::ostream& operator<<(
    std::ostream& out, const TabManagerDelegate::Candidate& candidate) {
  if (candidate.app()) {
    out << "app " << candidate.app()->pid() << " ("
        << candidate.app()->process_name() << ")"
        << ", process_state " << candidate.app()->process_state()
        << ", is_focused " << candidate.app()->is_focused()
        << ", lastActivityTime " << candidate.app()->last_activity_time();
  } else if (candidate.tab()) {
    out << "tab " << candidate.tab()->renderer_handle;
  }
  out << ", process_type " << candidate.process_type();
  return out;
}

TabManagerDelegate::Candidate& TabManagerDelegate::Candidate::operator=(
    TabManagerDelegate::Candidate&& other) {
  tab_ = other.tab_;
  app_ = other.app_;
  process_type_ = other.process_type_;
  return *this;
}

bool TabManagerDelegate::Candidate::operator<(
    const TabManagerDelegate::Candidate& rhs) const {
  if (process_type() != rhs.process_type())
    return process_type() < rhs.process_type();
  if (app() && rhs.app())
    return *app() < *rhs.app();
  if (tab() && rhs.tab())
    return TabManager::CompareTabStats(*tab(), *rhs.tab());
  // Impossible case. If app and tab are mixed in one process type, favor
  // apps.
  NOTREACHED() << "Undefined comparison between apps and tabs";
  return app();
}

ProcessType TabManagerDelegate::Candidate::GetProcessTypeInternal() const {
  if (app()) {
    if (app()->is_focused())
      return ProcessType::FOCUSED_APP;
    if (app()->process_state() == arc::mojom::ProcessState::TOP)
      return ProcessType::VISIBLE_APP;
    return ProcessType::BACKGROUND_APP;
  }
  if (tab()) {
    if (tab()->is_selected)
      return ProcessType::FOCUSED_TAB;
    return ProcessType::BACKGROUND_TAB;
  }
  NOTREACHED() << "Unexpected process type";
  return ProcessType::UNKNOWN_TYPE;
}

// Holds the info of a newly focused tab or app window. The focused process is
// set to highest priority (lowest OOM score), but not immediately. To avoid
// redundant settings the OOM score adjusting only happens after a timeout. If
// the process loses focus before the timeout, the adjustment is canceled.
class TabManagerDelegate::FocusedProcess {
 public:
  static const int kInvalidArcAppNspid = 0;

  FocusedProcess() { Reset(); }

  void SetTabPid(const base::ProcessHandle pid) {
    pid_ = pid;
    nspid_ = kInvalidArcAppNspid;
  }

  void SetArcAppNspid(const int nspid) {
    pid_ = base::kNullProcessHandle;
    nspid_ = nspid;
  }

  base::ProcessHandle GetTabPid() const { return pid_; }

  int GetArcAppNspid() const { return nspid_; }

  // Checks whether the containing instance is an ARC app. If so it resets the
  // data and returns true. Useful when canceling an ongoing OOM score setting
  // for a focused ARC app because the focus has been shifted away shortly.
  bool ResetIfIsArcApp() {
    if (nspid_ != kInvalidArcAppNspid) {
      Reset();
      return true;
    }
    return false;
  }

 private:
  void Reset() {
    pid_ = base::kNullProcessHandle;
    nspid_ = kInvalidArcAppNspid;
  }

  // The focused app could be a Chrome tab or an Android app, but not both.
  // At most one of them contains a valid value at any time.

  // If a chrome tab.
  base::ProcessHandle pid_;
  // If an Android app.
  int nspid_;
};

// TabManagerDelegate::MemoryStat implementation.

// static
int TabManagerDelegate::MemoryStat::ReadIntFromFile(
    const char* file_name, const int default_val) {
  std::string file_string;
  if (!base::ReadFileToString(base::FilePath(file_name), &file_string)) {
    LOG(WARNING) << "Unable to read file" << file_name;
    return default_val;
  }
  int val = default_val;
  if (!base::StringToInt(
          base::TrimWhitespaceASCII(file_string, base::TRIM_TRAILING),
          &val)) {
    LOG(WARNING) << "Unable to parse string" << file_string;
    return default_val;
  }
  return val;
}

// static
int TabManagerDelegate::MemoryStat::LowMemoryMarginKB() {
  static const int kDefaultLowMemoryMarginMb = 50;
  static const char kLowMemoryMarginConfig[] =
      "/sys/kernel/mm/chromeos-low_mem/margin";
  return ReadIntFromFile(
      kLowMemoryMarginConfig, kDefaultLowMemoryMarginMb) * 1024;
}

// The logic of available memory calculation is copied from
// _is_low_mem_situation() in kernel file include/linux/low-mem-notify.h.
// Maybe we should let kernel report the number directly.
int TabManagerDelegate::MemoryStat::TargetMemoryToFreeKB() {
  static const int kRamVsSwapWeight = 4;
  static const char kMinFilelistConfig[] = "/proc/sys/vm/min_filelist_kbytes";

  base::SystemMemoryInfoKB system_mem;
  base::GetSystemMemoryInfo(&system_mem);
  const int file_mem_kb = system_mem.active_file + system_mem.inactive_file;
  const int min_filelist_kb = ReadIntFromFile(kMinFilelistConfig, 0);
  // Calculate current available memory in system.
  // File-backed memory should be easy to reclaim, unless they're dirty.
  const int available_mem_kb = system_mem.free +
      file_mem_kb - system_mem.dirty - min_filelist_kb +
      system_mem.swap_free / kRamVsSwapWeight;

  return LowMemoryMarginKB() - available_mem_kb;
}

int TabManagerDelegate::MemoryStat::EstimatedMemoryFreedKB(
    base::ProcessHandle pid) {
  std::unique_ptr<base::ProcessMetrics> process_metrics(
      base::ProcessMetrics::CreateProcessMetrics(pid));
  base::WorkingSetKBytes mem_usage;
  process_metrics->GetWorkingSetKBytes(&mem_usage);
  return mem_usage.priv;
}

class TabManagerDelegate::UmaReporter {
 public:
  UmaReporter() : total_kills_(0) {}
  ~UmaReporter() {}

  void ReportKill(const int memory_freed);

 private:
  base::Time last_kill_time_;
  int total_kills_;
};

void TabManagerDelegate::UmaReporter::ReportKill(const int memory_freed) {
  base::Time now = base::Time::Now();
  const TimeDelta time_delta =
      last_kill_time_.is_null() ?
      TimeDelta::FromSeconds(arc::kMaxOomMemoryKillTimeDeltaSecs) :
      (now - last_kill_time_);
  UMA_HISTOGRAM_OOM_KILL_TIME_INTERVAL(
            "Arc.LowMemoryKiller.TimeDelta", time_delta);
  last_kill_time_ = now;

  ++total_kills_;
  UMA_HISTOGRAM_CUSTOM_COUNTS(
      "Arc.LowMemoryKiller.Count", total_kills_, 1, 1000, 1001);

  UMA_HISTOGRAM_MEMORY_KB("Arc.LowMemoryKiller.FreedSize",
                          memory_freed);
}

TabManagerDelegate::TabManagerDelegate(
    const base::WeakPtr<TabManager>& tab_manager)
  : TabManagerDelegate(tab_manager, new MemoryStat()) {
}

TabManagerDelegate::TabManagerDelegate(
    const base::WeakPtr<TabManager>& tab_manager,
    TabManagerDelegate::MemoryStat* mem_stat)
    : tab_manager_(tab_manager),
      focused_process_(new FocusedProcess()),
      mem_stat_(mem_stat),
      uma_(new UmaReporter()),
      weak_ptr_factory_(this) {
  registrar_.Add(this, content::NOTIFICATION_RENDERER_PROCESS_CLOSED,
                 content::NotificationService::AllBrowserContextsAndSources());
  registrar_.Add(this, content::NOTIFICATION_RENDERER_PROCESS_TERMINATED,
                 content::NotificationService::AllBrowserContextsAndSources());
  registrar_.Add(this, content::NOTIFICATION_RENDER_WIDGET_VISIBILITY_CHANGED,
                 content::NotificationService::AllBrowserContextsAndSources());
  auto* activation_client = GetActivationClient();
  if (activation_client)
    activation_client->AddObserver(this);
  BrowserList::GetInstance()->AddObserver(this);
}

TabManagerDelegate::~TabManagerDelegate() {
  BrowserList::GetInstance()->RemoveObserver(this);
  auto* activation_client = GetActivationClient();
  if (activation_client)
    activation_client->RemoveObserver(this);
}

void TabManagerDelegate::OnBrowserSetLastActive(Browser* browser) {
  // Set OOM score to the selected tab when a browser window is activated.
  // content::NOTIFICATION_RENDER_WIDGET_VISIBILITY_CHANGED didn't catch the
  // case (like when switching focus between 2 browser windows) so we need to
  // handle it here.
  TabStripModel* tab_strip_model = browser->tab_strip_model();
  int selected_index = tab_strip_model->active_index();
  content::WebContents* contents =
      tab_strip_model->GetWebContentsAt(selected_index);
  if (!contents)
    return;

  base::ProcessHandle pid = contents->GetRenderProcessHost()->GetHandle();
  AdjustFocusedTabScore(pid);
}

// TODO(cylee): Remove this function if Android process OOM score settings
// is moved back to Android.
// For example, negotiate non-overlapping OOM score ranges so Chrome and Android
// can set OOM score for processes in their own world.
void TabManagerDelegate::OnWindowActivated(
    aura::client::ActivationChangeObserver::ActivationReason reason,
    aura::Window* gained_active,
    aura::Window* lost_active) {
  if (IsArcWindow(gained_active)) {
    // Currently there is no way to know which app is displayed in the ARC
    // window, so schedule an early adjustment for all processes to reflect
    // the change.
    // Put a dummy FocusedProcess with nspid = kInvalidArcAppNspid for now to
    // indicate the focused process is an arc app.
    // TODO(cylee): Fix it when we have nspid info in ARC windows.
    focused_process_->SetArcAppNspid(FocusedProcess::kInvalidArcAppNspid);
    // If the timer is already running (possibly for a tab), it'll be reset
    // here.
    focus_process_score_adjust_timer_.Start(
        FROM_HERE,
        TimeDelta::FromMilliseconds(kFocusedProcessScoreAdjustIntervalMs),
        this, &TabManagerDelegate::ScheduleEarlyOomPrioritiesAdjustment);
  }
  if (IsArcWindow(lost_active)) {
    // Do not bother adjusting OOM score if the ARC window is deactivated
    // shortly.
    if (focused_process_->ResetIfIsArcApp() &&
        focus_process_score_adjust_timer_.IsRunning())
      focus_process_score_adjust_timer_.Stop();
  }
}

void TabManagerDelegate::ScheduleEarlyOomPrioritiesAdjustment() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (tab_manager_) {
    AdjustOomPriorities(tab_manager_->GetUnsortedTabStats());
  }
}

// If able to get the list of ARC procsses, prioritize tabs and apps as a whole.
// Otherwise try to kill tabs only.
void TabManagerDelegate::LowMemoryKill(
    const TabStatsList& tab_list) {
  arc::ArcProcessService* arc_process_service = arc::ArcProcessService::Get();
  if (arc_process_service &&
      arc_process_service->RequestAppProcessList(
          base::Bind(&TabManagerDelegate::LowMemoryKillImpl,
                     weak_ptr_factory_.GetWeakPtr(), tab_list))) {
    // LowMemoryKillImpl will be called asynchronously so nothing left to do.
    return;
  }
  // If the list of ARC processes is not available, call LowMemoryKillImpl
  // synchronously with an empty list of apps.
  std::vector<arc::ArcProcess> dummy_apps;
  LowMemoryKillImpl(tab_list, dummy_apps);
}

int TabManagerDelegate::GetCachedOomScore(ProcessHandle process_handle) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  auto it = oom_score_map_.find(process_handle);
  if (it != oom_score_map_.end()) {
    return it->second;
  }
  // An impossible value for oom_score_adj.
  return -1001;
}

void TabManagerDelegate::OnFocusTabScoreAdjustmentTimeout() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  base::ProcessHandle pid = focused_process_->GetTabPid();
  // The focused process doesn't render a tab. Could happen when the focus
  // just switched to an ARC app before the timeout. We can not avoid the race.
  if (pid == base::kNullProcessHandle)
    return;

  // Update the OOM score cache.
  oom_score_map_[pid] = chrome::kLowestRendererOomScore;

  // Sets OOM score.
  VLOG(3) << "Set OOM score " << chrome::kLowestRendererOomScore
          << " for focused tab " << pid;
  std::map<int, int> dict;
  dict[pid] = chrome::kLowestRendererOomScore;
  GetDebugDaemonClient()->SetOomScoreAdj(dict, base::Bind(&OnSetOomScoreAdj));
}

void TabManagerDelegate::AdjustFocusedTabScore(base::ProcessHandle pid) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  // Clear running timer if one was set for a previous focused tab/app.
  if (focus_process_score_adjust_timer_.IsRunning())
    focus_process_score_adjust_timer_.Stop();
  focused_process_->SetTabPid(pid);

  // If the currently focused tab already has a lower score, do not
  // set it. This can happen in case the newly focused tab is script
  // connected to the previous tab.
  ProcessScoreMap::iterator it = oom_score_map_.find(pid);
  const bool not_lowest_score = (it == oom_score_map_.end() ||
                                 it->second != chrome::kLowestRendererOomScore);

  if (not_lowest_score) {
    // By starting a timer we guarantee that the tab is focused for
    // certain amount of time. Secondly, it also does not add overhead
    // to the tab switching time.
    // If there's an existing running timer (could be for ARC app), it
    // would be replaced by a new task.
    focus_process_score_adjust_timer_.Start(
        FROM_HERE,
        TimeDelta::FromMilliseconds(kFocusedProcessScoreAdjustIntervalMs),
        this, &TabManagerDelegate::OnFocusTabScoreAdjustmentTimeout);
  }
}

void TabManagerDelegate::Observe(int type,
                                 const content::NotificationSource& source,
                                 const content::NotificationDetails& details) {
  switch (type) {
    case content::NOTIFICATION_RENDERER_PROCESS_CLOSED:
    case content::NOTIFICATION_RENDERER_PROCESS_TERMINATED: {
      content::RenderProcessHost* host =
          content::Source<content::RenderProcessHost>(source).ptr();
      oom_score_map_.erase(host->GetHandle());
      // Coming here we know that a renderer was just killed and memory should
      // come back into the pool. However - the memory pressure observer did
      // not yet update its status and therefore we ask it to redo the
      // measurement, calling us again if we have to release more.
      // Note: We do not only accelerate the discarding speed by doing another
      // check in short succession - we also accelerate it because the timer
      // driven MemoryPressureMonitor will continue to produce timed events
      // on top. So the longer the cleanup phase takes, the more tabs will
      // get discarded in parallel.
      base::chromeos::MemoryPressureMonitor* monitor =
          base::chromeos::MemoryPressureMonitor::Get();
      if (monitor)
        monitor->ScheduleEarlyCheck();
      break;
    }
    case content::NOTIFICATION_RENDER_WIDGET_VISIBILITY_CHANGED: {
      bool visible = *content::Details<bool>(details).ptr();
      if (visible) {
        content::RenderProcessHost* render_host =
            content::Source<content::RenderWidgetHost>(source)
            .ptr()
            ->GetProcess();
        AdjustFocusedTabScore(render_host->GetHandle());
      }
      // Do not handle the "else" case when it changes to invisible because
      // 1. The behavior is a bit awkward in that when switching from tab A to
      // tab B, the event "invisible of B" comes after "visible of A". It can
      // cause problems when the 2 tabs have the same content (e.g., New Tab
      // Page). To be more clear, if we try to cancel the timer when losing
      // focus it may cancel the timer for the same renderer process.
      // 2. When another window is launched on top of an existing browser
      // window, the selected tab in the existing browser didn't receive this
      // event, so an attempt to cancel timer in this case doesn't work.
      break;
    }
    default:
      NOTREACHED() << "Received unexpected notification";
      break;
  }
}

// Here we collect most of the information we need to sort the existing
// renderers in priority order, and hand out oom_score_adj scores based on that
// sort order.
//
// Things we need to collect on the browser thread (because
// TabStripModel isn't thread safe):
// 1) whether or not a tab is pinned
// 2) last time a tab was selected
// 3) is the tab currently selected
void TabManagerDelegate::AdjustOomPriorities(const TabStatsList& tab_list) {
  if (IsArcMemoryManagementEnabled()) {
    arc::ArcProcessService* arc_process_service = arc::ArcProcessService::Get();
    if (arc_process_service &&
        arc_process_service->RequestAppProcessList(
            base::Bind(&TabManagerDelegate::AdjustOomPrioritiesImpl,
                       weak_ptr_factory_.GetWeakPtr(), tab_list))) {
      return;
    }
  }
  // Pass in a dummy list if unable to get ARC processes.
  AdjustOomPrioritiesImpl(tab_list, std::vector<arc::ArcProcess>());
}

// Excludes persistent ARC apps, but still preserves active chrome tabs and
// focused ARC apps. The latter ones should not be killed by TabManager here,
// but we want to adjust their oom_score_adj.
// static
std::vector<TabManagerDelegate::Candidate>
TabManagerDelegate::GetSortedCandidates(
    const TabStatsList& tab_list,
    const std::vector<arc::ArcProcess>& arc_processes) {
  std::vector<Candidate> candidates;
  candidates.reserve(tab_list.size() + arc_processes.size());

  for (const auto& tab : tab_list) {
    candidates.emplace_back(&tab);
  }

  // A special process on Android side which serves as a dummy "focused" app
  // when the focused window is a Chrome side window (i.e., all Android
  // processes are running in the background). We don't want to kill it anyway.
  static constexpr char kArcInBackgroundDummyprocess[] =
      "org.chromium.arc.home";

  for (const auto& app : arc_processes) {
    // Skip persistent android processes since they should never be killed here.
    // Neither do we set their OOM scores so their score remains minimum.
    if (app.process_state() <= arc::mojom::ProcessState::PERSISTENT_UI ||
        app.process_name() == kArcInBackgroundDummyprocess) {
      continue;
    }
    candidates.emplace_back(&app);
  }

  // Sort candidates according to priority.
  std::sort(candidates.begin(), candidates.end());

  return candidates;
}

bool TabManagerDelegate::KillArcProcess(const int nspid) {
  auto* arc_bridge_service = arc::ArcBridgeService::Get();
  if (!arc_bridge_service)
    return false;

  auto* arc_process_instance =
      arc_bridge_service->process()->GetInstanceForMethod(
          "KillProcess", kMinVersionForKillProcess);
  if (!arc_process_instance)
    return false;

  arc_process_instance->KillProcess(nspid, "LowMemoryKill");
  return true;
}

bool TabManagerDelegate::KillTab(int64_t tab_id) {
  // Check |tab_manager_| is alive before taking tabs into consideration.
  return tab_manager_ &&
      tab_manager_->CanDiscardTab(tab_id) &&
      tab_manager_->DiscardTabById(tab_id);
}


chromeos::DebugDaemonClient* TabManagerDelegate::GetDebugDaemonClient() {
  return chromeos::DBusThreadManager::Get()->GetDebugDaemonClient();
}

void TabManagerDelegate::LowMemoryKillImpl(
    const TabStatsList& tab_list,
    const std::vector<arc::ArcProcess>& arc_processes) {

  VLOG(2) << "LowMemoryKilleImpl";
  const std::vector<TabManagerDelegate::Candidate> candidates =
      GetSortedCandidates(tab_list, arc_processes);

  int target_memory_to_free_kb = mem_stat_->TargetMemoryToFreeKB();
  // Kill processes until the estimated amount of freed memory is sufficient to
  // bring the system memory back to a normal level.
  // The list is sorted by descending importance, so we go through the list
  // backwards.
  for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
    VLOG(3) << "Target memory to free: " << target_memory_to_free_kb << " KB";
    // Never kill selected tab or Android foreground app, regardless whether
    // they're in the active window. Since the user experience would be bad.
    ProcessType process_type = it->process_type();
    if (process_type == ProcessType::VISIBLE_APP ||
        process_type == ProcessType::FOCUSED_APP ||
        process_type == ProcessType::FOCUSED_TAB) {
      VLOG(2) << "Skipped killing " << *it;
      continue;
    }
    if (it->app()) {
      int estimated_memory_freed_kb =
          mem_stat_->EstimatedMemoryFreedKB(it->app()->pid());
      if (KillArcProcess(it->app()->nspid())) {
        target_memory_to_free_kb -= estimated_memory_freed_kb;
        uma_->ReportKill(estimated_memory_freed_kb);
        VLOG(2) << "Killed " << *it;
      }
    } else {
      int64_t tab_id = it->tab()->tab_contents_id;
      // The estimation is problematic since multiple tabs may share the same
      // process, while the calculation counts memory used by the whole process.
      // So |estimated_memory_freed_kb| is an over-estimation.
      int estimated_memory_freed_kb =
          mem_stat_->EstimatedMemoryFreedKB(it->tab()->renderer_handle);
      if (KillTab(tab_id)) {
        target_memory_to_free_kb -= estimated_memory_freed_kb;
        uma_->ReportKill(estimated_memory_freed_kb);
        VLOG(2) << "Killed " << *it;
      }
    }
    if (target_memory_to_free_kb < 0)
      break;
  }
}

void TabManagerDelegate::AdjustOomPrioritiesImpl(
    const TabStatsList& tab_list,
    const std::vector<arc::ArcProcess>& arc_processes) {
  // Least important first.
  const auto candidates = GetSortedCandidates(tab_list, arc_processes);

  // Now we assign priorities based on the sorted list. We're assigning
  // priorities in the range of kLowestRendererOomScore to
  // kHighestRendererOomScore (defined in chrome_constants.h). oom_score_adj
  // takes values from -1000 to 1000. Negative values are reserved for system
  // processes, and we want to give some room below the range we're using to
  // allow for things that want to be above the renderers in priority, so the
  // defined range gives us some variation in priority without taking up the
  // whole range. In the end, however, it's a pretty arbitrary range to use.
  // Higher values are more likely to be killed by the OOM killer.

  // Break the processes into 2 parts. This is to help lower the chance of
  // altering OOM score for many processes on any small change.
  int range_middle =
      (chrome::kLowestRendererOomScore + chrome::kHighestRendererOomScore) / 2;

  // Find some pivot point. For now processes with priority >= CHROME_INTERNAL
  // are prone to be affected by LRU change. Taking them as "high priority"
  // processes.
  auto lower_priority_part = candidates.end();
  for (auto it = candidates.begin(); it != candidates.end(); ++it) {
    if (it->process_type() >= ProcessType::BACKGROUND_APP) {
      lower_priority_part = it;
      break;
    }
  }

  ProcessScoreMap new_map;

  // Higher priority part.
  DistributeOomScoreInRange(candidates.begin(), lower_priority_part,
                            chrome::kLowestRendererOomScore, range_middle,
                            &new_map);
  // Lower priority part.
  DistributeOomScoreInRange(lower_priority_part, candidates.end(), range_middle,
                            chrome::kHighestRendererOomScore, &new_map);
  oom_score_map_.swap(new_map);
}

void TabManagerDelegate::DistributeOomScoreInRange(
    std::vector<TabManagerDelegate::Candidate>::const_iterator begin,
    std::vector<TabManagerDelegate::Candidate>::const_iterator end,
    int range_begin,
    int range_end,
    ProcessScoreMap* new_map) {
  // Processes whose OOM scores should be updated. Ignore duplicated pids but
  // the last occurrence.
  std::map<base::ProcessHandle, int32_t> oom_scores_to_change;

  // Though there might be duplicate process handles, it doesn't matter to
  // overestimate the number of processes here since the we don't need to
  // use up the full range.
  int num = (end - begin);
  const float priority_increment =
      static_cast<float>(range_end - range_begin) / num;

  float priority = range_begin;
  for (auto cur = begin; cur != end; ++cur) {
    int score = static_cast<int>(priority + 0.5f);

    base::ProcessHandle pid = base::kNullProcessHandle;
    if (cur->app()) {
      pid = cur->app()->pid();
    } else {
      pid = cur->tab()->renderer_handle;
      // 1. tab_list contains entries for already-discarded tabs. If the PID
      // (renderer_handle) is zero, we don't need to adjust the oom_score.
      // 2. Only add unseen process handle so if there's multiple tab maps to
      // the same process, the process is set to an OOM score based on its "most
      // important" tab.
      if (pid == base::kNullProcessHandle ||
          new_map->find(pid) != new_map->end())
        continue;
    }

    if (pid == base::kNullProcessHandle)
      continue;

    // Update the to-be-cached OOM score map. Use pid as map keys so it's
    // globally unique.
    (*new_map)[pid] = score;

    // Need to update OOM score if the calculated score is different from
    // current cached score.
    if (oom_score_map_[pid] != score) {
      VLOG(3) << "Update OOM score " << score << " for " << *cur;
      oom_scores_to_change[pid] = static_cast<int32_t>(score);
    }
    priority += priority_increment;
  }

  if (oom_scores_to_change.size())
    GetDebugDaemonClient()->SetOomScoreAdj(
        oom_scores_to_change, base::Bind(&OnSetOomScoreAdj));
}

}  // namespace memory
