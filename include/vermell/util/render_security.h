//
// Security knobs for the file-rendering family (readFile, compose, render).
//

#ifndef VERMELL_RENDER_SECURITY_H
#define VERMELL_RENDER_SECURITY_H

#include <cstddef>
#include <string>

namespace vermell {

    struct RenderSecurity {
        // Jail: every path handed to readFile/file/compose/render must
        // resolve (symlinks included) inside this directory.
        // Empty = no jail (legacy behavior, NOT recommended for production).
        std::string root{};

        // Maximum bytes a single rendered file may occupy in memory.
        size_t max_file_bytes = 32UL * 1024UL * 1024UL;
    };

    // Effective jail root for the file-rendering family. An empty configured
    // root falls back to the working directory, so a server that never set
    // .render.root is still jailed to its launch directory instead of being
    // able to serve any file on the machine (open-by-default legacy behavior
    // was a Local File Inclusion foot-gun). Set .root explicitly to a
    // dedicated public/ directory in production.
    [[nodiscard]] inline std::string effective_root(const RenderSecurity& sec) {
        return sec.root.empty() ? std::string(".") : sec.root;
    }

} // namespace vermell

#endif // VERMELL_RENDER_SECURITY_H
