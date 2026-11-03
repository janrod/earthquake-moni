// The earthquake pages: list, then detail. Touch handling and state only, no drawing or network.
// Host-testable: no Arduino dependencies.
#pragma once
#include <stdint.h>
#include <string.h>

// Recognises a drag starting at the right edge and moving left, which pulls the earthquake
// list in over the live page. It doesn't claim the top edge, which opens the settings panel.
class EdgeSwipeDetector {
 public:
  static constexpr int kEdgeWidth = 70, kScreenW = 720, kTopEdge = 60, kCommit = 14;
  // True once, when a press has become this drag. startX() is where the finger came down.
  bool update(bool pressed, int x, int y) {
    bool fired = false;
    if (pressed && !wasPressed_) {
      startX_ = x;
      startY_ = y;
      armed_ = x >= kScreenW - kEdgeWidth && y > kTopEdge;
    } else if (pressed && armed_) {
      const int dx = startX_ - x, dy = y - startY_;
      const int ady = dy < 0 ? -dy : dy;
      if (ady > kCommit && ady >= dx) armed_ = false;  // a vertical move along the edge
      else if (dx >= kCommit) {
        armed_ = false;
        fired = true;
      }
    }
    wasPressed_ = pressed;
    return fired;
  }
  int startX() const { return startX_; }
  // For when something else has taken over the finger, so this never sees it lift.
  void reset() {
    wasPressed_ = false;
    armed_ = false;
  }

 private:
  bool wasPressed_ = false, armed_ = false;
  int startX_ = 0, startY_ = 0;
};

// The live page and the list side by side, following the finger. 0 shows all of the live page,
// and kFull all of the list. The content moves with the finger, like natural scrolling.
class PageSlide {
 public:
  static constexpr int kFull = 720, kDecide = 180, kPxPerMs = 3;
  void begin(int fromPos, int fingerX, uint32_t now) {
    active_ = true;
    settling_ = false;
    from_ = pos_ = fromPos;
    grabX_ = fingerX;
    lastNow_ = now;
  }
  void update(bool pressed, int x, uint32_t now) {
    const uint32_t dt = now - lastNow_;
    lastNow_ = now;
    if (!active_) return;
    if (settling_) {
      int step = (int)(dt > 200 ? 200 : dt) * kPxPerMs;
      if (pos_ < target_) pos_ = pos_ + step > target_ ? target_ : pos_ + step;
      else pos_ = pos_ - step < target_ ? target_ : pos_ - step;
      if (pos_ == target_) {
        active_ = false;
        finished_ = true;
      }
    } else if (pressed) {
      const int p = from_ + (grabX_ - x);
      pos_ = p < 0 ? 0 : p > kFull ? kFull : p;
    } else {
      // It takes a deliberate pull to change page; a short one springs back.
      const int moved = pos_ - from_;
      target_ = (moved >= kDecide) ? kFull : (moved <= -kDecide) ? 0 : from_;
      settling_ = true;
    }
  }
  void cancel() { active_ = false; }
  bool active() const { return active_; }
  int position() const { return pos_; }
  // True once when it comes to rest; listShown() says on which side.
  bool takeFinished() {
    const bool f = finished_;
    finished_ = false;
    return f;
  }
  bool listShown() const { return pos_ == kFull; }

 private:
  bool active_ = false, settling_ = false, finished_ = false;
  int from_ = 0, pos_ = 0, target_ = 0, grabX_ = 0;
  uint32_t lastNow_ = 0;
};

class QuakeBrowser {
 public:
  enum class View { kHidden, kList, kDetail };
  enum class Load { kIdle, kLoading, kFailed, kReady };
  struct Rect {
    int x, y, w, h;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };
  static constexpr int kScreenH = 1280;
  static constexpr int kHeaderH = 100, kFilterH = 76, kListTop = kHeaderH + kFilterH, kFooterH = 44, kRowH = 118;
  static constexpr int kMaxRows = 24000;
  // Filter buttons: 0 is All, and the rest are intensity ranks 1-9 (see quake_data.h).
  static constexpr int kFilterCount = 10, kFilterW = 72;
  static constexpr int kTapSlop = 24, kSwipeDistance = 140;
  // A row lights up only once the finger has stayed still this long, so starting a scroll
  // doesn't flash whichever row it began on. A quick tap still opens at once.
  static constexpr uint32_t kHighlightDelayMs = 120;
  static constexpr uint32_t kRefreshMs = 60 * 1000;  // the server's own cache time
  static constexpr uint32_t kRetryMs = 5 * 1000;
  // Older than this, new rows are likely enough that the list waits for them rather than jumping.
  static constexpr uint32_t kStaleListMs = 10 * 60 * 1000;
  static constexpr uint32_t kListWaitMaxMs = 10 * 1000;  // a slow network mustn't hide the rows for long
  static constexpr uint32_t kSpinMs = 100;
  static constexpr Rect kBack{0, 0, 170, kHeaderH};
  static constexpr int kJsonNameSize = 64;

  void setAutoReturnMs(uint32_t ms) { autoReturnMs_ = ms; }
  // Room for kMaxRows + 2 entries. Too big for internal memory, so the caller provides it.
  void setRowStorage(int32_t *storage) { visibleRows_ = storage; }

  void open(uint32_t now) {
    view_ = View::kList;
    scrollY_ = 0;
    wasPressed_ = false;
    armedRow_ = -1;
    backArmed_ = false;
    lastActivity_ = now;
    dirty_ = true;
    requestListIfDue(now);
    // Not after a failure, as being offline shouldn't add a wait to every visit.
    const bool stale = !haveGoodList_ || now - lastGoodListMs_ >= kStaleListMs;
    if (stale && !listFailedRefresh_ && (listRequest_ || listLoad_ == Load::kLoading)) {
      listWaiting_ = true;
      listWaitFromMs_ = lastSpinMs_ = now;
    }
  }

  void close() {
    view_ = View::kHidden;
    dirty_ = true;
  }

  void update(bool pressed, int x, int y, uint32_t now) {
    if (view_ == View::kHidden) return;
    const bool down = pressed && !wasPressed_;
    const bool up = !pressed && wasPressed_;
    wasPressed_ = pressed;
    if (pressed || up) lastActivity_ = now;
    if (down) {
      pressedAt_ = now;
      begin(x, y);
    } else if (pressed) {
      move(x, y);
    } else if (up) {
      end();
    }
    if (pressed && armedRow_ >= 0 && !rowLit_ && now - pressedAt_ >= kHighlightDelayMs) {
      rowLit_ = true;
      dirty_ = true;
    }
    if (view_ == View::kList) requestListIfDue(now);
    if (listWaiting_ && now - listWaitFromMs_ >= kListWaitMaxMs) {
      listWaiting_ = false;
      dirty_ = true;
    } else if (listWaiting_ && now - lastSpinMs_ >= kSpinMs) {
      lastSpinMs_ = now;
      dirty_ = true;
    }
    if (view_ != View::kHidden && autoReturnMs_ && now - lastActivity_ >= autoReturnMs_) close();
  }

  // After a slide has handed the list back: the finger has been up for a while.
  void resume(uint32_t now) {
    wasPressed_ = false;
    lastActivity_ = now;
    dirty_ = true;
  }
  // A rightward drag on the list, for the page slide to take over. Gives where it began.
  bool takeBackDrag(int *fingerX) {
    if (!backDrag_) return false;
    backDrag_ = false;
    *fingerX = grabX_;
    return true;
  }

  // --- results from the network side ---
  void listLoaded(bool ok, int rows, uint32_t now) {
    listLoad_ = ok ? Load::kReady : (rowCount_ > 0 ? Load::kReady : Load::kFailed);
    listWaiting_ = false;
    if (ok) {
      haveGoodList_ = true;
      lastGoodListMs_ = now;
      rowCount_ = rows > kMaxRows ? kMaxRows : rows;
      rebuildVisible();
      const int m = maxScroll();
      if (scrollY_ > m) scrollY_ = m;
    }
    lastListMs_ = now;
    haveListTime_ = true;
    listFailedRefresh_ = !ok;
    dirty_ = true;
  }
  // New older rows have been merged in. The row count is the new total.
  void olderLoaded(bool ok, int rows) {
    olderLoad_ = ok ? Load::kReady : Load::kFailed;
    if (ok) {
      rowCount_ = rows > kMaxRows ? kMaxRows : rows;
      rebuildVisible();
    }
    dirty_ = true;
  }
  void setOlderEnabled(bool enabled) {
    if (enabled != olderEnabled_) dirty_ = true;
    olderEnabled_ = enabled;
  }
  // The network side has started a load by itself.
  void olderStarted() {
    olderLoad_ = Load::kLoading;
    dirty_ = true;
  }
  void detailLoaded(bool ok) {
    detailLoad_ = ok ? Load::kReady : Load::kFailed;
    dirty_ = true;
  }

  // --- requests for the network side ---
  bool takeListRequest() { return take(listRequest_); }
  bool takeOlderRequest() { return take(olderRequest_); }
  bool takeDetailRequest(char *jsonName, int *row) {
    if (!detailRequest_) return false;
    detailRequest_ = false;
    strcpy(jsonName, selectedJson_);
    *row = selectedRow_;
    return true;
  }
  // The row's report file, as the list has it. Set by the caller when the list changes.
  void setRowJsonLookup(const char *(*lookup)(int row)) { rowJson_ = lookup; }
  void setRowRankLookup(int (*lookup)(int row)) { rowRank_ = lookup; }
  void setRowPartialLookup(bool (*lookup)(int row)) { rowPartial_ = lookup; }

  // --- state for drawing ---
  bool takeDirty() { return take(dirty_); }
  View view() const { return view_; }
  bool visible() const { return view_ != View::kHidden; }
  int rowCount() const { return rowCount_; }
  // Rows that pass the filter; `i` here and in rowRect() and armedRow() counts only those.
  int visibleCount() const { return visibleCount_; }
  // -1 is the divider, above the first row older than the point the list is complete back to.
  // It also loads more: the row at the very end can be thousands of rows away.
  int visibleRow(int i) const { return visibleRows_[i]; }
  int dividerIndex() const { return dividerIndex_; }
  bool filterOn(int button) const { return button == 0 ? filterMask_ == 0 : (filterMask_ >> button) & 1; }
  int armedFilter() const { return armedFilter_; }
  // The row after the last earthquake is "Load older". With nothing to show, row 0 says so.
  int olderRowIndex() const { return visibleCount_ == 0 ? 1 : visibleCount_; }
  Load olderLoad() const { return olderLoad_; }
  bool olderEnabled() const { return olderEnabled_; }
  unsigned filterMask() const { return filterMask_; }
  int scrollY() const { return scrollY_; }
  int armedRow() const { return armedRow_; }
  int litRow() const { return rowLit_ ? armedRow_ : -1; }
  bool backArmed() const { return backArmed_; }
  int selectedRow() const { return selectedRow_; }
  const char *selectedJson() const { return selectedJson_; }
  Load listLoad() const { return listLoad_; }
  // Rows are held back, with a spinner up, until the refresh they might jump for arrives.
  bool listWaiting() const { return listWaiting_; }
  // Something on the page has changed with time alone.
  void redraw() { dirty_ = true; }
  Load detailLoad() const { return detailLoad_; }
  bool listRefreshFailed() const { return listFailedRefresh_; }

  static Rect rowRect(int i, int scrollY) { return Rect{0, kListTop + i * kRowH - scrollY, 720, kRowH}; }
  static Rect filterRect(int button) { return Rect{button * kFilterW, kHeaderH, kFilterW, kFilterH}; }

 private:
  static bool take(bool &flag) {
    const bool v = flag;
    flag = false;
    return v;
  }

  int maxScroll() const {
    const int content = (olderRowIndex() + 1) * kRowH, view = kScreenH - kListTop - kFooterH;
    return content > view ? content - view : 0;
  }

  void requestListIfDue(uint32_t now) {
    if (listLoad_ == Load::kLoading) return;
    // A list that never loaded is worth retrying soon; a good one only when it can have changed.
    if (haveListTime_ && now - lastListMs_ < (listLoad_ == Load::kFailed ? kRetryMs : kRefreshMs)) return;
    if (listLoad_ != Load::kReady) listLoad_ = Load::kLoading;
    lastListMs_ = now;
    haveListTime_ = true;
    listRequest_ = true;
    dirty_ = true;
  }

  void setArmedRow(int r) {
    if (r != armedRow_ && rowLit_) dirty_ = true;
    if (r != armedRow_) rowLit_ = false;
    armedRow_ = r;
  }
  void setBackArmed(bool b) {
    if (backArmed_ != b) dirty_ = true;
    backArmed_ = b;
  }

  int rowAt(int y) const {
    if (y < kListTop || y >= kScreenH - kFooterH) return -1;
    const int i = (y - kListTop + scrollY_) / kRowH;
    if (i == olderRowIndex() && rowCount_ > 0) return i;
    return i >= 0 && i < visibleCount_ ? i : -1;
  }

  void rebuildVisible() {
    visibleCount_ = 0;
    dividerIndex_ = -1;
    bool dividerDone = !rowPartial_;
    for (int i = 0; i < rowCount_; i++) {
      const int rank = filterMask_ && rowRank_ ? rowRank_(i) : 0;
      if (filterMask_ != 0 && !(rank > 0 && ((filterMask_ >> rank) & 1))) continue;
      if (!dividerDone && rowPartial_(i)) {
        dividerDone = true;
        dividerIndex_ = visibleCount_;
        visibleRows_[visibleCount_++] = -1;
      }
      visibleRows_[visibleCount_++] = i;
    }
  }

  void setArmedFilter(int f) {
    if (armedFilter_ != f) dirty_ = true;
    armedFilter_ = f;
  }

  // A level toggles on and off; All clears them, and clearing the last one means All again.
  void pressFilter(int button) {
    filterMask_ = button == 0 ? 0 : filterMask_ ^ (1u << button);
    rebuildVisible();
    scrollY_ = 0;
    if (olderLoad_ != Load::kLoading) olderLoad_ = Load::kIdle;
    dirty_ = true;
  }

  void begin(int x, int y) {
    grabX_ = lastX_ = x;
    grabY_ = lastY_ = y;
    grabScroll_ = scrollY_;
    axis_ = Axis::kNone;
    if (kBack.contains(x, y)) setBackArmed(true);
    else if (view_ == View::kList && y >= kHeaderH && y < kListTop) setArmedFilter(x / kFilterW < kFilterCount ? x / kFilterW : -1);
    else if (view_ == View::kList && !listWaiting_) setArmedRow(rowAt(y));
  }

  void move(int x, int y) {
    lastX_ = x;
    lastY_ = y;
    const int dx = x - grabX_, dy = y - grabY_;
    const int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (axis_ == Axis::kNone && (adx > kTapSlop || ady > kTapSlop)) {
      axis_ = adx > ady ? Axis::kHorizontal : Axis::kVertical;
      setBackArmed(false);
      setArmedRow(-1);
      setArmedFilter(-1);
      if (axis_ == Axis::kHorizontal && dx > 0 && view_ == View::kList) backDrag_ = true;
    }
    if (axis_ == Axis::kVertical && view_ == View::kList && !listWaiting_) {
      int s = grabScroll_ - dy;
      const int m = maxScroll();
      s = s < 0 ? 0 : s > m ? m : s;
      if (s != scrollY_) dirty_ = true;
      scrollY_ = s;
    }
  }

  void goBack() {
    if (view_ == View::kDetail) {
      view_ = View::kList;
      dirty_ = true;
    } else {
      close();
    }
  }

  void end() {
    const bool back = backArmed_;
    const int row = armedRow_;
    const int filter = armedFilter_;
    setBackArmed(false);
    setArmedRow(-1);
    setArmedFilter(-1);
    if (axis_ == Axis::kHorizontal) {
      // Pages sit to the right of the one before, so swiping right goes back.
      if (view_ == View::kDetail && lastX_ - grabX_ >= kSwipeDistance) goBack();
      return;
    }
    if (back) {
      goBack();
    } else if (filter >= 0) {
      pressFilter(filter);
    } else if (row >= 0 && (row == olderRowIndex() || visibleRows_[row] < 0)) {
      if (olderEnabled_ && olderLoad_ != Load::kLoading) {
        olderLoad_ = Load::kLoading;
        olderRequest_ = true;
        dirty_ = true;
      }
    } else if (row >= 0 && rowJson_) {
      const int real = visibleRows_[row];
      const char *json = rowJson_(real);
      if (!json || !json[0]) return;
      strncpy(selectedJson_, json, kJsonNameSize - 1);
      selectedJson_[kJsonNameSize - 1] = 0;
      selectedRow_ = real;
      detailLoad_ = Load::kLoading;
      detailRequest_ = true;
      view_ = View::kDetail;
      dirty_ = true;
    }
  }

  enum class Axis { kNone, kHorizontal, kVertical };

  View view_ = View::kHidden;
  Load listLoad_ = Load::kIdle, detailLoad_ = Load::kIdle;
  bool listFailedRefresh_ = false;
  Load olderLoad_ = Load::kIdle;
  bool olderEnabled_ = false, olderRequest_ = false;
  int rowCount_ = 0, scrollY_ = 0, grabX_ = 0, grabY_ = 0, lastX_ = 0, lastY_ = 0, grabScroll_ = 0;
  int armedRow_ = -1, selectedRow_ = -1, armedFilter_ = -1;
  bool rowLit_ = false;
  uint32_t pressedAt_ = 0;
  int visibleCount_ = 0, dividerIndex_ = -1;
  int32_t *visibleRows_ = nullptr;
  unsigned filterMask_ = 0;
  int (*rowRank_)(int) = nullptr;
  bool (*rowPartial_)(int) = nullptr;
  bool backArmed_ = false, wasPressed_ = false;
  Axis axis_ = Axis::kNone;
  uint32_t lastActivity_ = 0, lastListMs_ = 0, autoReturnMs_ = 10000;
  bool listWaiting_ = false, haveGoodList_ = false;
  uint32_t listWaitFromMs_ = 0, lastSpinMs_ = 0, lastGoodListMs_ = 0;
  bool haveListTime_ = false, dirty_ = false, listRequest_ = false, detailRequest_ = false, backDrag_ = false;
  char selectedJson_[kJsonNameSize] = {};
  const char *(*rowJson_)(int) = nullptr;
};
