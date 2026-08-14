#!/usr/bin/env python3
"""
apply_convert_iterator_fix.py

Fixes the emf-blob "chopped off / content pushed outside the viewBox" bug:
convert_iterator() was reapplying the original <image> placeholder's (x,y)
offset on top of emf2svg output that's sometimes already rendered in final,
page-absolute coordinates. This inserts bounding-box/overlap helper
functions and replaces convert_iterator() with a version that only keeps
the translate when it doesn't make the fit against the viewBox worse.

This locates code by function signature and brace-matching (not by line
number or literal whitespace), so it's safe to run regardless of
tabs-vs-spaces or any other formatting differences in your checkout --
same approach as apply_scale_title_fix.py.

Usage:
    python3 apply_convert_iterator_fix.py /path/to/src/lib/visio2svg/Visio2Svg.cpp

Writes a Visio2Svg.cpp.bak2 backup before modifying anything. Safe to run
after (or before) apply_scale_title_fix.py -- the two touch different
functions and don't conflict.
"""

import re
import sys
from pathlib import Path

INCLUDES_BLOCK = """#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>
"""

HELPERS_MARKER = "BEGIN translate-safety helpers"

HELPERS_BLOCK = '''// ===========================================================================
// BEGIN translate-safety helpers (fix: erroneous emf-blob translate() that
// pushes already-correctly-positioned content outside the document viewBox)
// ===========================================================================
//
// Background: convert_iterator() wraps each converted emf/wmf blob in a
// <g transform="translate(x,y)"> using x,y taken directly from the original
// <image> placeholder node. That's correct when the converted content is a
// small icon meant to sit at an offset within a larger scene. But some EMF
// blobs are already rendered by emf2svg in absolute, page-filling
// coordinates -- for those, re-applying the placeholder's offset shifts the
// whole drawing out of the SVG's viewBox (most visibly cutting off the top
// when y is a large negative number).
//
// The helpers below compute the actual bounding box of the converted
// content (correctly walking any nested transform="matrix(...)" the
// converter itself emits), compare it against the document's viewBox with
// and without the proposed translate, and only keep the translate when it
// doesn't make the fit meaningfully worse. This mirrors the same heuristic
// used in fix_emf_translate.py, now applied at conversion time instead of
// as a postprocessing pass.
//
// NOTE: this block is inserted directly inside the existing
// `namespace visio2svg { ... }` in this file -- it intentionally does NOT
// open its own namespace.

struct Mat2D {
    double a, b, c, d, e, f;
};

static const Mat2D MAT_IDENTITY = {1, 0, 0, 1, 0, 0};

static Mat2D mat_compose(const Mat2D &p, const Mat2D &c) {
    Mat2D r;
    r.a = p.a * c.a + p.c * c.b;
    r.b = p.b * c.a + p.d * c.b;
    r.c = p.a * c.c + p.c * c.d;
    r.d = p.b * c.c + p.d * c.d;
    r.e = p.a * c.e + p.c * c.f + p.e;
    r.f = p.b * c.e + p.d * c.f + p.f;
    return r;
}

static void mat_apply(const Mat2D &m, double x, double y, double &ox,
                      double &oy) {
    ox = m.a * x + m.c * y + m.e;
    oy = m.b * x + m.d * y + m.f;
}

// Parses a transform="..." attribute value (matrix/translate/scale; rotate
// and skew are not expected from emf2svg's output and are intentionally
// left unhandled) into a single composed affine matrix.
static Mat2D parse_transform(const char *value) {
    Mat2D result = MAT_IDENTITY;
    if (!value)
        return result;
    std::string s(value);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t open = s.find('(', pos);
        if (open == std::string::npos)
            break;
        std::string func = s.substr(pos, open - pos);
        while (!func.empty() && std::isspace((unsigned char)func.front()))
            func.erase(func.begin());
        while (!func.empty() && std::isspace((unsigned char)func.back()))
            func.pop_back();
        size_t close = s.find(')', open);
        if (close == std::string::npos)
            break;
        std::string args = s.substr(open + 1, close - open - 1);
        // args may be comma- and/or whitespace-separated
        std::replace(args.begin(), args.end(), ',', ' ');
        std::vector<double> nums;
        {
            std::stringstream ss(args);
            double v;
            while (ss >> v)
                nums.push_back(v);
        }
        Mat2D local = MAT_IDENTITY;
        if (func == "matrix" && nums.size() == 6) {
            local.a = nums[0];
            local.b = nums[1];
            local.c = nums[2];
            local.d = nums[3];
            local.e = nums[4];
            local.f = nums[5];
        } else if (func == "translate") {
            local.e = nums.size() > 0 ? nums[0] : 0.0;
            local.f = nums.size() > 1 ? nums[1] : 0.0;
        } else if (func == "scale") {
            double sx = nums.size() > 0 ? nums[0] : 1.0;
            double sy = nums.size() > 1 ? nums[1] : sx;
            local.a = sx;
            local.d = sy;
        }
        result = mat_compose(result, local);
        pos = close + 1;
    }
    return result;
}

struct BBox {
    double minx, miny, maxx, maxy;
    bool valid;
};

static void bbox_extend(BBox &b, double x, double y) {
    if (!b.valid) {
        b.minx = b.maxx = x;
        b.miny = b.maxy = y;
        b.valid = true;
        return;
    }
    if (x < b.minx)
        b.minx = x;
    if (x > b.maxx)
        b.maxx = x;
    if (y < b.miny)
        b.miny = y;
    if (y > b.maxy)
        b.maxy = y;
}

// Small SVG path 'd' bounding-box parser. Handles M/L/H/V/C/S/Q/T/A/Z,
// upper and lower case. Curve control points are folded into the bbox
// (slightly looser than the true rendered bbox); that's fine here since
// this is only used to decide whether a translate is harmful, not for
// rendering.
static BBox parse_path_bbox(const char *d) {
    BBox bbox = {0, 0, 0, 0, false};
    if (!d)
        return bbox;

    double cx = 0, cy = 0, sx = 0, sy = 0;
    char cmd = 0;
    std::vector<double> nums;

    auto flush = [&]() {
        if (!cmd)
            return;
        char upper = (char)std::toupper((unsigned char)cmd);
        bool rel = std::islower((unsigned char)cmd);
        if (upper == 'Z') {
            cx = sx;
            cy = sy;
            return;
        }
        int argc = 0;
        switch (upper) {
        case 'M':
        case 'L':
        case 'T':
            argc = 2;
            break;
        case 'H':
        case 'V':
            argc = 1;
            break;
        case 'C':
            argc = 6;
            break;
        case 'S':
        case 'Q':
            argc = 4;
            break;
        case 'A':
            argc = 7;
            break;
        default:
            argc = 0;
        }
        if (argc == 0)
            return;
        bool first = true;
        for (size_t i = 0; i + (size_t)argc <= nums.size(); i += argc) {
            if (upper == 'M' || upper == 'L' || upper == 'T') {
                double x = nums[i], y = nums[i + 1];
                if (rel) {
                    x += cx;
                    y += cy;
                }
                cx = x;
                cy = y;
                if (upper == 'M' && first) {
                    sx = cx;
                    sy = cy;
                }
                bbox_extend(bbox, cx, cy);
            } else if (upper == 'H') {
                double x = nums[i];
                cx = rel ? cx + x : x;
                bbox_extend(bbox, cx, cy);
            } else if (upper == 'V') {
                double y = nums[i];
                cy = rel ? cy + y : y;
                bbox_extend(bbox, cx, cy);
            } else if (upper == 'C' || upper == 'S' || upper == 'Q') {
                for (int j = 0; j < argc; j += 2) {
                    double x = nums[i + j], y = nums[i + j + 1];
                    if (rel) {
                        x += cx;
                        y += cy;
                    }
                    bbox_extend(bbox, x, y);
                    if (j == argc - 2) {
                        cx = x;
                        cy = y;
                    }
                }
            } else if (upper == 'A') {
                double x = nums[i + 5], y = nums[i + 6];
                if (rel) {
                    x += cx;
                    y += cy;
                }
                cx = x;
                cy = y;
                bbox_extend(bbox, cx, cy);
            }
            first = false;
        }
    };

    const char *p = d;
    while (*p) {
        if (std::strchr("MmLlHhVvCcSsQqTtAaZz", *p)) {
            flush();
            cmd = *p;
            nums.clear();
            p++;
        } else if (std::isdigit((unsigned char)*p) || *p == '-' ||
                   *p == '+' || *p == '.') {
            char *end;
            double v = strtod(p, &end);
            if (end == p) {
                p++;
                continue;
            }
            nums.push_back(v);
            p = end;
        } else {
            p++;
        }
    }
    flush();
    return bbox;
}

// Recursively walks a parsed XML fragment (as produced by emf2svg /
// wmf2svg), composing nested transform="matrix(...)"/"translate(...)"/
// "scale(...)" attributes, and merges in the bounding box of every <path>
// element found.
static void compute_content_bbox(xmlNode *node, const Mat2D &parentMat,
                                 BBox &out) {
    for (xmlNode *n = node; n; n = n->next) {
        if (n->type != XML_ELEMENT_NODE)
            continue;

        xmlChar *transformAttr = xmlGetProp(n, (const xmlChar *)"transform");
        Mat2D localMat = transformAttr
                             ? parse_transform((const char *)transformAttr)
                             : MAT_IDENTITY;
        if (transformAttr)
            xmlFree(transformAttr);
        Mat2D combined = mat_compose(parentMat, localMat);

        if (!xmlStrcmp(n->name, (const xmlChar *)"path")) {
            xmlChar *dAttr = xmlGetProp(n, (const xmlChar *)"d");
            if (dAttr) {
                BBox local = parse_path_bbox((const char *)dAttr);
                if (local.valid) {
                    double x0, y0, x1, y1, x2, y2, x3, y3;
                    mat_apply(combined, local.minx, local.miny, x0, y0);
                    mat_apply(combined, local.minx, local.maxy, x1, y1);
                    mat_apply(combined, local.maxx, local.miny, x2, y2);
                    mat_apply(combined, local.maxx, local.maxy, x3, y3);
                    bbox_extend(out, x0, y0);
                    bbox_extend(out, x1, y1);
                    bbox_extend(out, x2, y2);
                    bbox_extend(out, x3, y3);
                }
                xmlFree(dAttr);
            }
        }

        if (n->children)
            compute_content_bbox(n->children, combined, out);
    }
}

static bool get_root_viewbox(xmlDocPtr root_doc, double &vx, double &vy,
                             double &vw, double &vh) {
    xmlNode *root = xmlDocGetRootElement(root_doc);
    if (!root)
        return false;
    xmlChar *vb = xmlGetProp(root, (const xmlChar *)"viewBox");
    if (!vb)
        return false;
    int n = sscanf((const char *)vb, "%lf %lf %lf %lf", &vx, &vy, &vw, &vh);
    xmlFree(vb);
    return n == 4;
}

// Returns true if `content` already covers a large portion of the document
// viewBox on its own, and applying (tx,ty) would make its overlap with the
// viewBox meaningfully worse -- i.e. this translate is very likely the
// "placeholder offset re-applied to already-final-position content" bug
// rather than an intentional small-icon placement, which should be left
// alone.
static bool translate_is_harmful(const BBox &content, double vx, double vy,
                                 double vw, double vh, double tx, double ty) {
    if (!content.valid || vw <= 0 || vh <= 0)
        return false;

    double contentW = content.maxx - content.minx;
    double contentH = content.maxy - content.miny;
    double contentArea = contentW * contentH;
    if (contentArea <= 0)
        return false;

    double viewArea = vw * vh;
    double relativeSize = contentArea / viewArea;

    auto overlapFraction = [&](double ox0, double oy0, double ox1,
                               double oy1) {
        double ix0 = std::max(ox0, vx), iy0 = std::max(oy0, vy);
        double ix1 = std::min(ox1, vx + vw), iy1 = std::min(oy1, vy + vh);
        double iw = std::max(0.0, ix1 - ix0), ih = std::max(0.0, iy1 - iy0);
        return (iw * ih) / contentArea;
    };

    double overlapBefore =
        overlapFraction(content.minx, content.miny, content.maxx, content.maxy);
    double overlapAfter =
        overlapFraction(content.minx + tx, content.miny + ty,
                        content.maxx + tx, content.maxy + ty);

    const double LARGE_CONTENT_THRESHOLD = 0.5; // >=50% of viewBox area
    const double WORSE_OVERLAP_DROP = 0.15;     // >=15pt overlap drop

    return (relativeSize >= LARGE_CONTENT_THRESHOLD) &&
           ((overlapBefore - overlapAfter) >= WORSE_OVERLAP_DROP);
}

// ===========================================================================
// END translate-safety helpers
// ===========================================================================

'''

NEW_CONVERT_ITERATOR = '''int convert_iterator(xmlNode *a_node, xmlDocPtr root_doc) {
    xmlNode *cur_node = NULL;
    xmlNode *next_node;
    int ret = 0;

    for (cur_node = a_node; cur_node;) {
        next_node = cur_node->next;

        // image node specific treatment
        if ((!xmlStrcmp(cur_node->name, (const xmlChar *)"image"))) {
            xmlAttr *attribute = cur_node->properties;
            double x = 0;
            double y = 0;
            double width = 0;
            double height = 0;
            xmlChar *imgb64 = NULL;
            // recover some attributes needed for the conversion and the base64
            // encoded image.
            while (attribute) {
                xmlChar *value =
                    xmlNodeListGetString(cur_node->doc, attribute->children, 1);
                if ((!xmlStrcmp(attribute->name, (const xmlChar *)"x"))) {
                    x = atof((const char *)value);
                }
                if ((!xmlStrcmp(attribute->name, (const xmlChar *)"y"))) {
                    y = atof((const char *)value);
                }
                if ((!xmlStrcmp(attribute->name, (const xmlChar *)"width"))) {
                    width = atof((const char *)value);
                }
                if ((!xmlStrcmp(attribute->name, (const xmlChar *)"height"))) {
                    height = atof((const char *)value);
                }
                if ((!xmlStrcmp(attribute->name, (const xmlChar *)"href"))) {
                    imgb64 = value;
                    attribute = attribute->next;
                    continue;
                }
                attribute = attribute->next;
                xmlFree(value);
            }

            // detect the image type
            // right now, we handle emf and wmf
            // if image_type stays equal to UNKNOWN_IMGTYPE, it's not a type we
            // handle
            IMG_TYPE image_type = UNKNOWN_IMGTYPE;
            if (imgb64 != NULL) {
                if ((!xmlStrncmp(imgb64,
                                 (const xmlChar *)"data:image/emf;base64,",
                                 22))) {
                    image_type = EMF_IMGTYPE;
                } else if ((!xmlStrncmp(
                               imgb64,
                               (const xmlChar *)"data:image/wmf;base64,",
                               22))) {
                    image_type = WMF_IMGTYPE;
                }
            }

            // if the image is something we handle, convert it to SVG
            // else, just free the imgb64 and keep the node
            if (image_type != UNKNOWN_IMGTYPE) {
                xmlAttr *attribute = cur_node->properties;

                // recover content (base64 encoded)
                char *svg_out = NULL;
                size_t len_out = 0;
                size_t len = strlen((char const *)imgb64);
                size_t size = len; //(len * 3 / 4 + 4);
                unsigned char *content = (unsigned char *)calloc(size, 1);
                int b64e = base64decode((char *)(imgb64 + 22), (len - 22),
                                        content, &size);
                ret |= b64e;
                xmlFree(imgb64);
                imgb64 = NULL;
                int e2se;
                xmlNodePtr s_sta;
                xmlNodePtr s_end;
                if (b64e) {
#ifdef DEBUG
                    std::cerr << "ERROR: Base64 decode failed" << std::endl;
#endif
                    ret = 1;
                }

                switch (image_type) {
                case EMF_IMGTYPE: {
                    // configure generator options
                    generatorOptions *options =
                        (generatorOptions *)calloc(1, sizeof(generatorOptions));
                    options->verbose = false;
                    options->emfplus = true;
                    // options->nameSpace = (char *)"svg";
                    options->nameSpace = NULL;
                    options->svgDelimiter = false;
                    options->imgWidth = width;
                    options->imgHeight = height;

                    // convert emf
                    e2se = emf2svg((char *)content, size, &svg_out, &len_out,
                                   options);
                    if (!e2se) {
#ifdef DEBUG
                        std::cerr << "ERROR: Failed to convert emf blob"
                                  << std::endl;
#endif
                        ret = 1;
                    }
                    free(options);
                    s_sta = xmlNewDocComment(
                        root_doc, (const unsigned char *)"emf-blob start");
                    s_end = xmlNewDocComment(
                        root_doc, (const unsigned char *)"emf-blob end");
                    break;
                }
                case WMF_IMGTYPE: {
                    e2se = wmf2svg_draw((char *)content, size, width, height,
                                        &svg_out, &len_out);
                    s_sta = xmlNewDocComment(
                        root_doc, (const unsigned char *)"wmf-blob start");
                    s_end = xmlNewDocComment(
                        root_doc, (const unsigned char *)"wmf-blob end");
                    if (e2se) {
#ifdef DEBUG
                        std::cerr << "ERROR: Failed to convert wmf blob"
                                  << std::endl;
#endif
                        ret = 1;
                    }
                    break;
                }
                default: {
#ifdef DEBUG
                    std::cerr << "ERROR: Unknown image type" << std::endl;
#endif
                    ret = 1;
                    break;
                }
                }

                xmlDocPtr doc;
                xmlNode *root_element = NULL;

                // parse svg generated by emf2svg or wmf2svg with libxml2
                {
                    // emf2svg's raw output can be multiple sibling top-level
                    // <g> blocks rather than a single rooted document, which
                    // is not well-formed XML. Wrap it in one enclosing tag
                    // before parsing so nothing after the first block gets
                    // silently discarded as "extra content".
                    int wrapped_len = len_out + 6; // "<g>" + "</g>"
                    char *wrapped = (char *)malloc(wrapped_len + 1);
                    memcpy(wrapped, "<g>", 3);
                    memcpy(wrapped + 3, svg_out, len_out);
                    memcpy(wrapped + 3 + len_out, "</g>", 4);
                    wrapped[wrapped_len] = '\\0';
                    doc = xmlReadMemory(wrapped, wrapped_len, NULL, NULL,
                                        XML_PARSE_RECOVER | XML_PARSE_NOBLANKS |
                                            XML_PARSE_NONET | XML_PARSE_NOERROR |
                                            XML_PARSE_HUGE);
                    free(wrapped);
                }
                root_element = xmlDocGetRootElement(doc);

                // Decide whether to actually apply the placeholder's (x,y)
                // translate, now that we can see where emf2svg/wmf2svg
                // actually rendered the content. If the rendered content
                // already fills most of the document's viewBox on its own,
                // and applying (x,y) would push it substantially outside
                // that viewBox, treat the translate as harmful and use
                // (0,0) instead. See translate_is_harmful() above.
                double final_x = x;
                double final_y = y;
                if (root_element) {
                    BBox contentBBox = {0, 0, 0, 0, false};
                    compute_content_bbox(root_element, MAT_IDENTITY,
                                         contentBBox);
                    double vx, vy, vw, vh;
                    if (get_root_viewbox(root_doc, vx, vy, vw, vh) &&
                        translate_is_harmful(contentBBox, vx, vy, vw, vh, x,
                                             y)) {
                        final_x = 0.0;
                        final_y = 0.0;
                    }
                }

                // create an svg group node (emf conversion will be put in it)
                xmlNode *node = xmlNewNode(NULL, (const xmlChar *)"g");

                // prepare translate to position emf conversion
                // (inside transform attribute), using final_x/final_y
                // rather than the raw placeholder x/y (see above).
                size_t tlen = (size_t)snprintf(NULL, 0, " translate(%f,%f)  ",
                                               final_x, final_y);
                char *translate = (char *)malloc(tlen);
                tlen = snprintf(translate, tlen, " translate(%f,%f)  ",
                                final_x, final_y);
                bool translate_set = false;

                // copy all attributes of the image node in the group node
                // except a few
                while (attribute) {
                    if (xmlStrcmp(attribute->name, (const xmlChar *)"href") &&
                        xmlStrcmp(attribute->name, (const xmlChar *)"x") &&
                        xmlStrcmp(attribute->name, (const xmlChar *)"y") &&
                        xmlStrcmp(attribute->name, (const xmlChar *)"width") &&
                        xmlStrcmp(attribute->name, (const xmlChar *)"height")) {
                        xmlChar *value = xmlNodeListGetString(
                            cur_node->doc, attribute->children, 1);
                        // special treatement for transform, must append the
                        // translate
                        // previously prepared
                        if ((!xmlStrcmp(attribute->name,
                                        (const xmlChar *)"transform"))) {
                            translate_set = true;
                            value = xmlStrncat(
                                value, (const xmlChar *)translate, tlen);
                        }
                        xmlNewProp(node, attribute->name, value);
                    }
                    attribute = attribute->next;
                }
                // if there was no "transform" attribute, add it with our
                // translate
                if (!(translate_set)) {
                    xmlNewProp(node, (const xmlChar *)"transform",
                               (const xmlChar *)translate);
                }

                xmlAddChild(node, s_sta);

                // insert new nodes
                xmlNode *blob_svg = xmlCopyNodeList(root_element);
                xmlAddChildList(node, blob_svg);
                xmlAddChildList(cur_node->parent, node);
                xmlAddChild(node, s_end);

                // remove image node
                xmlUnlinkNode(cur_node);

                // freeing some memory
                xmlFreeNode(cur_node);
                free(content);
                free(svg_out);
                free(translate);
                xmlFreeDoc(doc);
            } else {
                ret |= convert_iterator(cur_node->children, root_doc);
            }
            free(imgb64);
        } else {
            ret |= convert_iterator(cur_node->children, root_doc);
        }
        cur_node = next_node;
    }
    return ret;
}'''


def find_function_span(text: str, start_idx: int):
    brace_open = text.find("{", start_idx)
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
                return (start_idx, i + 1)
        i += 1
    return None


def main():
    if len(sys.argv) != 2:
        print("Usage: python3 apply_convert_iterator_fix.py /path/to/Visio2Svg.cpp", file=sys.stderr)
        sys.exit(1)

    path = Path(sys.argv[1])
    if not path.is_file():
        print(f"Not a file: {path}", file=sys.stderr)
        sys.exit(1)

    text = path.read_text(encoding="utf-8")
    original_text = text

    # --- 1. Insert required #includes above the namespace, if not present ---
    if "#include <algorithm>" not in text:
        ns_match = re.search(r"namespace\s+visio2svg\s*\{", text)
        if not ns_match:
            print("ERROR: could not find 'namespace visio2svg {' to insert includes before.", file=sys.stderr)
            sys.exit(1)
        insert_at = ns_match.start()
        text = text[:insert_at] + INCLUDES_BLOCK + "\n" + text[insert_at:]

    # --- 2. Locate convert_iterator() by signature + brace matching ---
    sig_pattern = re.compile(r"int\s+convert_iterator\s*\(")
    m = sig_pattern.search(text)
    if not m:
        print("ERROR: could not find 'int convert_iterator(' in this file.", file=sys.stderr)
        sys.exit(1)

    span = find_function_span(text, m.start())
    if span is None:
        print("ERROR: could not find the matching closing brace of convert_iterator().", file=sys.stderr)
        sys.exit(1)
    func_start, func_end = span

    # --- 3. Insert the helper block immediately before convert_iterator, ---
    #        unless it's already present (idempotent re-run).
    if HELPERS_MARKER not in text:
        text = text[:func_start] + HELPERS_BLOCK + text[func_start:]
        # re-locate convert_iterator now that text shifted
        m = sig_pattern.search(text)
        span = find_function_span(text, m.start())
        func_start, func_end = span

    # --- 4. Replace convert_iterator's body with the fixed version ---
    old_function = text[func_start:func_end]
    if "final_x" in old_function:
        print("convert_iterator() already looks patched (found 'final_x'). No changes made.")
        sys.exit(0)

    new_text = text[:func_start] + NEW_CONVERT_ITERATOR + text[func_end:]

    if new_text == original_text:
        print("No changes were necessary.")
        sys.exit(0)

    backup_path = path.with_suffix(path.suffix + ".bak2")
    backup_path.write_text(original_text, encoding="utf-8")
    path.write_text(new_text, encoding="utf-8")

    print(f"Backed up original to: {backup_path}")
    print(f"Patched: {path}")
    print("Inserted translate-safety helpers and replaced convert_iterator().")
    print("Please review the file and rebuild.")


if __name__ == "__main__":
    main()
