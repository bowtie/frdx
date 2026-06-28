// FastBoot: skips FH6's startup black-screen hold by hooking
// timing APIs and jumping the virtual clock past the busy-wait deadline.
// Ported from TigerHix/fh6_fastboot (MIT).
//
// The skip is scoped to the intro video's lifetime (BinkOpen -> BinkClose)
// and auto-calibrates the spin threshold per machine.  F8 = manual skip.
// Config is read from the [fastboot] section of config.toml.

#pragma once

namespace frdx {
namespace fastboot {

// Install timer/file hooks and start the monitor thread.  Must be called
// early (from DllMain, before the game's startup sequence begins).
void start() noexcept;

// Returns true once the intro video has closed (or fastboot is disabled /
// never started).  The overlay uses this to suppress non-console UI during
// the startup sequence.
bool intro_done() noexcept;

} // namespace fastboot
} // namespace frdx
