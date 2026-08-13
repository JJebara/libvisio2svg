#!/usr/bin/env python3
"""
apply_scale_title_fix.py

Locates Visio2Svg::scale_title() in Visio2Svg.cpp by its signature and
brace-matching (NOT by literal line-for-line text matching), and replaces
the entire function body with a corrected version. This avoids the
tabs-vs-spaces / line-ending brittleness of a positional `patch` file.

Fixes two bugs in the original function:
  1. width/height unit suffix (e.g. "in") was silently dropped by atof(),
     causing SVG viewers to treat the bare number as pixels.
  2. Content was wrapped in an extra <g transform="scale(scaling)">
     on top of already-scaled width/height, double-applying the scale
     factor and pushing all path coordinates outside the (unscaled)
     viewBox for any -s value other than 1.

Usage:
    python3 apply_scale_title_fix.py /path/to/src/lib/visio2svg/Visio2Svg.cpp

Writes a Visio2Svg.cpp.bak backup next to the original before modifying it.
"""

import re
import sys
from pathlib import Path

NEW_FUNCTION = '''int Visio2Svg::scale_title(xmlNode **root, xmlDocPtr *doc, double scaling,
                           const xmlChar *title, int title_len) {
    int ret = 0;

    xmlNode *new_root = xmlNewNode(NULL, (const xmlChar *)"svg");
    xmlAttr *attribute = (*root)->properties;
    while (attribute) {
        xmlChar *value = xmlNodeListGetString(*doc, attribute->children, 1);
        if ((!xmlStrcmp(attribute->name, (const xmlChar *)"width")) ||
            (!xmlStrcmp(attribute->name, (const xmlChar *)"height"))) {
            // atof() only consumes the leading numeric portion of the
            // string. librevenge emits width/height with a unit suffix
            // (e.g. "0.317in"), so capture whatever text trails the
            // number via strtod()'s endptr and re-append it -- otherwise
            // the unit is silently dropped and the resulting bare number
            // gets interpreted by SVG viewers as unitless pixels.
            char *endptr = NULL;
            double geom = strtod((const char *)value, &endptr);
            char *cgeom = (char *)calloc(1, 64);
            snprintf(cgeom, 64, "%.10f%s", geom * scaling, endptr);
            xmlNewProp(new_root, attribute->name, (const xmlChar *)cgeom);
            free(cgeom);
        } else {
            xmlNewProp(new_root, attribute->name, value);
        }
        attribute = attribute->next;
        xmlFree(value);
    }

    xmlNsPtr ns = (*root)->nsDef;
    while (ns) {
        xmlNewNs(new_root, ns->href, ns->prefix);
        ns = ns->next;
    }

    xmlDocPtr new_doc = xmlCopyDoc(*doc, 0);
    xmlNodePtr title_cdata = xmlNewCDataBlock(new_doc, title, title_len);
    xmlNodePtr title_node =
        xmlNewChild(new_root, NULL, (const xmlChar *)"title", NULL);
    xmlAddChild(title_node, title_cdata);
    xmlDocSetRootElement(new_doc, new_root);
    // Do NOT also wrap the content in a scale() transform here. width/
    // height above are already multiplied by `scaling`, which on its own
    // is the complete, correct way to resize an SVG's physical/display
    // size: viewers automatically stretch the (unchanged) viewBox
    // coordinate space to fill whatever width/height is given. Adding a
    // second scale() transform on top of that re-applies the same
    // factor twice, so every path coordinate ends up `scaling`x outside
    // the untouched viewBox for any scaling != 1.0 -- i.e. the whole
    // drawing renders off-canvas (blank) whenever -s is used with a
    // value other than 1.
    xmlNode *children = xmlCopyNodeList((*root)->children);
    xmlAddChildList(new_root, children);
    xmlFreeDoc(*doc);
    *doc = new_doc;
    *root = new_root;
    return ret;
}'''


def find_function_span(text: str, signature_start: str):
    """Find [start, end) indices of the function whose text begins with
    signature_start, by scanning forward from the first '{' after the
    signature and tracking brace depth until it returns to zero."""
    sig_idx = text.find(signature_start)
    if sig_idx == -1:
        return None

    brace_open = text.find("{", sig_idx)
    if brace_open == -1:
        return None

    depth = 0
    i = brace_open
    while i < len(text):
        ch = text[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return (sig_idx, i + 1)
        i += 1
    return None


def main():
    if len(sys.argv) != 2:
        print("Usage: python3 apply_scale_title_fix.py /path/to/Visio2Svg.cpp", file=sys.stderr)
        sys.exit(1)

    path = Path(sys.argv[1])
    if not path.is_file():
        print(f"Not a file: {path}", file=sys.stderr)
        sys.exit(1)

    text = path.read_text(encoding="utf-8")

    # Match the signature loosely (function name + first parameter type),
    # tolerant of tabs/spaces/line-wrapping differences in the rest of
    # the signature.
    sig_pattern = re.compile(
        r"int\s+Visio2Svg::scale_title\s*\("
    )
    m = sig_pattern.search(text)
    if not m:
        print("ERROR: Could not find 'int Visio2Svg::scale_title(' in this file.", file=sys.stderr)
        print("Make sure you're pointing this at the right Visio2Svg.cpp.", file=sys.stderr)
        sys.exit(1)

    span = find_function_span(text, text[m.start():m.start() + 40])
    # Redo the span search anchored at the actual match start for accuracy
    depth = 0
    brace_open = text.find("{", m.start())
    if brace_open == -1:
        print("ERROR: Could not find the opening brace of scale_title().", file=sys.stderr)
        sys.exit(1)
    i = brace_open
    end = None
    while i < len(text):
        ch = text[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                end = i + 1
                break
        i += 1

    if end is None:
        print("ERROR: Could not find the matching closing brace of scale_title().", file=sys.stderr)
        sys.exit(1)

    start = m.start()
    old_function = text[start:end]

    if "translate" not in old_function and "atof" not in old_function:
        print("WARNING: the function body doesn't look like the version this fix targets.")
        print("Proceeding anyway, but please review the diff below carefully.\n")

    new_text = text[:start] + NEW_FUNCTION + text[end:]

    backup_path = path.with_suffix(path.suffix + ".bak")
    backup_path.write_text(text, encoding="utf-8")
    path.write_text(new_text, encoding="utf-8")

    print(f"Backed up original to: {backup_path}")
    print(f"Patched: {path}")
    print(f"\nReplaced {len(old_function)} chars (old function) with {len(NEW_FUNCTION)} chars (new function).")
    print("Please review the file and rebuild.")


if __name__ == "__main__":
    main()
