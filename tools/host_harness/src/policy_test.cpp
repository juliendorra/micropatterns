// Off-device tests for the shared browse and refresh policy.
//
// Every case here is a bug that shipped and was found by watching a panel and
// guessing. They are cheap to run and they fail loudly, which is the entire
// argument for the two classes under test existing at all.
#include "mp_browse_policy.h"
#include "mp_refresh_budget.h"

#include <cstdio>
#include <cstdlib>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        g_checks++;                                                             \
        if (!(cond)) {                                                          \
            g_failures++;                                                       \
            printf("  FAIL  %s\n        at %s:%d\n", (msg), __FILE__, __LINE__);\
        }                                                                       \
    } while (0)

static const char* actionName(MpBrowseAction a)
{
    switch (a) {
        case MpBrowseAction::Nothing:   return "Nothing";
        case MpBrowseAction::ShowTitle: return "ShowTitle";
        case MpBrowseAction::Render:    return "Render";
    }
    return "?";
}

// A policy with 12 scripts, the shipping constants, and a clock we control.
struct Fixture {
    MpBrowsePolicy p;
    uint32_t now = 1000;
    uint32_t presses = 0;

    Fixture()
    {
        p.setScriptCount(12);
        p.setCurrent(0);
        p.renderFinished(now, true);
    }
    void advance(uint32_t ms) { now += ms; }
    MpBrowseAction poll(bool down = false) { return p.poll(now, down, presses); }
    // One press: the caller sees a release edge and asks to browse.
    MpBrowseAction press(int delta) { presses++; return p.browse(delta, now); }
};

// 1. A title must be readable. The window starts when the title is ON the
//    panel, not when it was asked for -- drawing it costs about as long as the
//    window itself, so arming first spent the whole window drawing.
static void test_settle_starts_when_the_title_is_up()
{
    printf("settle window starts when the title is drawn\n");
    Fixture f;

    CHECK(f.press(+1) == MpBrowseAction::ShowTitle, "a press asks for a title");
    CHECK(f.p.pending() == 1, "the pending selection stepped");
    CHECK(f.p.current() == 0, "the committed selection did not");

    // The panel takes 485ms to put the title up. Nothing may render meanwhile.
    f.advance(485);
    CHECK(f.poll() == MpBrowseAction::Nothing, "no render before the title is reported drawn");

    f.p.titleDrawn(f.now);
    f.advance(449);
    CHECK(f.poll() == MpBrowseAction::Nothing, "no render one millisecond early");
    f.advance(1);
    CHECK(f.poll() == MpBrowseAction::Render, "renders once the window has elapsed");
    CHECK(f.p.current() == 1, "the selection is committed by the render");
    CHECK(!f.p.browsing(), "and is no longer pending");
}

// 2. The periodic re-render must not repaint the outgoing script over a fresh
//    title, then render the chosen one straight after.
static void test_auto_rerun_never_preempts_a_title()
{
    printf("periodic re-render yields to a pending selection\n");
    Fixture f;

    // Sit idle until the periodic re-render is overdue, then press.
    f.advance(83000);
    CHECK(f.poll() == MpBrowseAction::Render, "overdue on its own");

    Fixture g;
    g.advance(82900);
    CHECK(g.press(+1) == MpBrowseAction::ShowTitle, "press just before the deadline");
    g.p.titleDrawn(g.now);
    g.advance(100);   // the deadline passes while the title is up
    CHECK(g.poll() == MpBrowseAction::Nothing, "the outgoing script is not repainted");
    g.advance(400);
    CHECK(g.poll() == MpBrowseAction::Render, "the chosen script renders");
    CHECK(g.p.current() == 1, "and it is the chosen one, not the outgoing one");
}

// 2b. ...and it must also yield to a press that has not been released yet.
//     A browse is only reported on the release edge, so between the two edges
//     the user has visibly started something while browsing() is still false.
//     Reported from the bench as: press next on the last script, get no title,
//     then watch the OUTGOING script re-render, then finally the chosen one.
static void test_auto_rerun_yields_to_a_button_still_down()
{
    printf("periodic re-render yields to a button that is still down\n");
    Fixture f;
    f.advance(83000);
    CHECK(f.poll(/*down=*/true) == MpBrowseAction::Nothing,
          "overdue, but a finger is on the button");
    CHECK(f.poll(true) == MpBrowseAction::Nothing, "still down, still nothing");

    // The real sequence: the press is released, becomes a browse, and the only
    // thing that renders is the script the user chose.
    CHECK(f.press(+1) == MpBrowseAction::ShowTitle, "the release becomes a browse");
    f.p.titleDrawn(f.now);
    f.advance(200);
    CHECK(f.poll() == MpBrowseAction::Nothing, "the title is still settling");
    f.advance(250);
    CHECK(f.poll() == MpBrowseAction::Render, "and then it renders");
    CHECK(f.p.current() == 1, "the chosen script, never the outgoing one");
}

// 3. A held button extends the window, because that is what makes paging feel
//    like paging -- but not forever. A contact that never reads low used to
//    hold the device on a title indefinitely, with the press indicator lit,
//    which is indistinguishable from a crash.
static void test_held_button_cannot_freeze_the_device()
{
    printf("a held button postpones the render, but not without limit\n");
    Fixture f;
    f.press(+1);
    f.p.titleDrawn(f.now);

    f.advance(450);
    CHECK(f.poll(/*down=*/true) == MpBrowseAction::Nothing, "held: postponed");
    f.advance(450);
    CHECK(f.poll(true) == MpBrowseAction::Nothing, "still held: still postponed");

    // Past the cap, the render proceeds whatever the pin says.
    f.advance(1500);
    CHECK(f.poll(true) == MpBrowseAction::Render, "past the cap it renders anyway");
    CHECK(f.p.current() == 1, "on the script the user chose");
}

// 4. A render is abandoned by a NEW press, not by a button that was already
//    down when it started. Otherwise the button that survives rule 3 goes on to
//    abort every render it triggers.
static void test_abort_needs_a_new_press_not_a_held_one()
{
    printf("only a new press abandons a render\n");
    Fixture f;
    f.press(+1);
    f.p.titleDrawn(f.now);
    // The cap runs from the moment the render first became due, so the first
    // blocked poll is what starts it; only the next one can be past the cap.
    f.advance(450);
    CHECK(f.poll(true) == MpBrowseAction::Nothing, "the first blocked poll starts the cap");
    f.advance(1500);
    CHECK(f.poll(true) == MpBrowseAction::Render, "renders past the hold cap");

    f.p.renderStarted(f.now, f.presses);
    CHECK(!f.p.abortRequested(f.presses), "the button still held is not an abort");
    f.presses++;   // a genuine new press arrives mid-render
    CHECK(f.p.abortRequested(f.presses), "a new press is");
}

// 5. An abandoned render is not a frame the user saw, so it must not push the
//    periodic deadline out; a run of interrupted renders would otherwise stop
//    time-dependent scripts from ever advancing.
static void test_abandoned_render_does_not_reset_the_clock()
{
    printf("an abandoned render does not count as a frame shown\n");
    Fixture f;
    f.advance(80000);
    f.p.renderStarted(f.now, f.presses);
    f.p.renderFinished(f.now, /*completed=*/false);
    f.advance(3000);
    CHECK(f.poll() == MpBrowseAction::Render, "the periodic deadline still stands");

    f.p.renderStarted(f.now, f.presses);
    f.p.renderFinished(f.now, true);
    f.advance(3000);
    CHECK(f.poll() == MpBrowseAction::Nothing, "a completed one does reset it");
}

// A burst of presses walks the list and renders only the script stopped on.
static void test_a_burst_renders_only_the_last()
{
    printf("a burst of presses renders once\n");
    Fixture f;
    for (int i = 0; i < 5; i++) {
        CHECK(f.press(+1) == MpBrowseAction::ShowTitle, "each press shows a title");
        f.p.titleDrawn(f.now);
        f.advance(300);                                    // faster than the window
        CHECK(f.poll() == MpBrowseAction::Nothing, "nothing renders mid-burst");
    }
    CHECK(f.p.pending() == 5, "the burst walked five steps");
    f.advance(450);
    CHECK(f.poll() == MpBrowseAction::Render, "the script stopped on renders");
    CHECK(f.p.current() == 5, "and it is the fifth");
}

static void test_wrapping_and_empty_list()
{
    printf("selection wraps, and an empty list does nothing\n");
    Fixture f;
    f.p.setCurrent(11);
    f.press(+1);
    CHECK(f.p.pending() == 0, "wraps forward past the end");
    f.p.setCurrent(0);
    f.p.browse(-1, f.now);
    CHECK(f.p.pending() == 11, "and backward past the start");

    MpBrowsePolicy empty;
    empty.setScriptCount(0);
    CHECK(empty.browse(+1, 0) == MpBrowseAction::Nothing, "no scripts: no title");
    CHECK(empty.poll(100000, false, 0) == MpBrowseAction::Nothing, "no scripts: no render");
}

// 6. The de-ghost flash must never land on a title. It is transient, and a
//    seconds-long flash in place of a name is the worst frame to spend it on.
static void test_deghost_never_lands_on_a_title()
{
    printf("the de-ghost flash never lands on a title frame\n");
    MpRefreshBudget b(24);
    b.reset();

    for (int i = 0; i < 23; i++) b.beginUpdate(false, true);
    CHECK(b.since() == 23, "23 updates charged");

    // The frame that would tip it over is a title. It must not flash.
    CHECK(b.beginUpdate(false, /*allowDeghost=*/false) == false, "a title never flashes");
    CHECK(b.beginUpdate(false, false) == false, "nor does the next one");
    CHECK(b.since() == 25, "but the debt keeps accruing");

    // The next frame that allows it pays, which is the one already being awaited.
    CHECK(b.beginUpdate(false, true) == true, "the following render pays the debt");
    CHECK(b.since() == 0, "and the budget resets");
}

static void test_budget_basics()
{
    printf("refresh budget accounting\n");
    MpRefreshBudget b(8);
    CHECK(b.beginUpdate(false, true) == true, "the first update of a session is full");
    // An interval of N is N partials BETWEEN full refreshes, so the full lands
    // on the N+1'th call after one.
    for (int i = 0; i < 8; i++) {
        CHECK(b.beginUpdate(false, true) == false, "partials in between");
    }
    CHECK(b.beginUpdate(false, true) == true, "full again after the interval");
    CHECK(b.beginUpdate(true, true) == true, "forceFull always flashes");
    CHECK(b.since() == 0, "and resets the budget");
}

int main()
{
    printf("\nBrowse and refresh policy\n\n");
    test_settle_starts_when_the_title_is_up();
    test_auto_rerun_never_preempts_a_title();
    test_auto_rerun_yields_to_a_button_still_down();
    test_held_button_cannot_freeze_the_device();
    test_abort_needs_a_new_press_not_a_held_one();
    test_abandoned_render_does_not_reset_the_clock();
    test_a_burst_renders_only_the_last();
    test_wrapping_and_empty_list();
    test_deghost_never_lands_on_a_title();
    test_budget_basics();

    printf("\nPOLICY: %d checks, %d failed\n\n", g_checks, g_failures);
    (void)actionName;
    return g_failures == 0 ? 0 : 1;
}
