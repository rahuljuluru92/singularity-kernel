#pragma once

namespace shell {

// The interactive command loop: prints a prompt, reads a line from
// serial input, parses and dispatches it, repeats forever. Meant to be
// the last thing kernel_main calls.
[[noreturn]] void run();

} // namespace shell
