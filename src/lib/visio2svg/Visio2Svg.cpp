/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */

#include "visio2svg/Visio2Svg.h"
#include "visio2svg/TitleGenerator.h"
#include <emf2svg.h>
#include <iostream>
#include <librevenge-generators/librevenge-generators.h>
#include <librevenge-stream/librevenge-stream.h>
#include <librevenge/librevenge.h>
#include <libvisio/libvisio.h>
#include <libwmf/api.h>
#include <libwmf/ipa.h>
#include <libwmf/svg.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <math.h>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <string>
#include <sys/types.h>
#include <unordered_map>

#ifdef DARWIN
#include "memstream.c"
#endif

#define VISIOVSS 1
#define VISIOVSD 2

#define WHITESPACE 64
#define EQUALS 65
#define INVALID 66
#define WMF2SVG_MAXPECT (1 << 0)

#include <cctype>
#include <cstring>
#include <vector>

namespace visio2svg {

static void or_wmf_svg_device_begin(wmfAPI *API) {
    wmf_svg_t *ddata = WMF_SVG_GetData(API);

    wmfStream *out = ddata->out;

    if (out == 0)
        return;

    if ((out->reset(out->context)) &&
        ((API->flags & WMF_OPT_IGNORE_NONFATAL) == 0)) {
        API->err = wmf_E_DeviceError;
        return;
    }

    if ((ddata->bbox.BR.x <= ddata->bbox.TL.x) ||
        (ddata->bbox.BR.y <= ddata->bbox.TL.y)) {
        API->err = wmf_E_Glitch;
        return;
    }

    if ((ddata->width == 0) || (ddata->height == 0)) {
        ddata->width = (unsigned int)ceil(ddata->bbox.BR.x - ddata->bbox.TL.x);
        ddata->height = (unsigned int)ceil(ddata->bbox.BR.y - ddata->bbox.TL.y);
    }

    wmf_stream_printf(API, out, (char *)"<g>\n");

    if (ddata->Description) {
        wmf_stream_printf(API, out, (char *)"<desc>%s</desc>\n",
                          ddata->Description);
    }
}

/* This is called from the end of each play for page termination
 */
static void or_wmf_svg_device_end(wmfAPI *API) {
    wmf_svg_t *ddata = WMF_SVG_GetData(API);

    wmfStream *out = ddata->out;
    wmf_stream_printf(API, out, (char *)"</g>\n");

    WMF_DEBUG(API, "~~~~~~~~wmf_[svg_]device_end");

    if (out == 0)
        return;
}

Visio2Svg::Visio2Svg() {
}

Visio2Svg::~Visio2Svg() {
}

typedef struct _ImageContext ImageContext;

struct _ImageContext {
    int number;
    char *prefix;
};

int Visio2Svg::vss2svg(std::string &in,
                       std::unordered_map<std::string, std::string> &out,
                       double scaling) {
    return visio2svg(in, out, scaling, VISIOVSS);
}

int Visio2Svg::vss2svg(std::string &in,
                       std::unordered_map<std::string, std::string> &out) {
    return visio2svg(in, out, 1.0, VISIOVSS);
}

int Visio2Svg::vsd2svg(std::string &in,
                       std::unordered_map<std::string, std::string> &out) {
    return visio2svg(in, out, 1.0, VISIOVSD);
}

int Visio2Svg::vsd2svg(std::string &in,
                       std::unordered_map<std::string, std::string> &out,
                       double scaling) {
    return visio2svg(in, out, scaling, VISIOVSD);
}

int explicit_wmf_error(char const *str, wmf_error_t err) {
    int status = 0;

    switch (err) {
    case wmf_E_None:
#ifdef DEBUG
        fprintf(stderr, "%s returned with wmf_E_None.\n", str);
#endif
        status = 0;
        break;

    case wmf_E_InsMem:
#ifdef DEBUG
        fprintf(stderr, "%s returned with wmf_E_InsMem.\n", str);
#endif
        status = 1;
        break;

    case wmf_E_BadFile:
#ifdef DEBUG
        fprintf(stderr, "%s returned with wmf_E_BadFile.\n", str);
#endif
        status = 1;
        break;

    case wmf_E_BadFormat:
#ifdef DEBUG
        fprintf(stderr, "%s returned with wmf_E_BadFormat.\n", str);
#endif
        status = 1;
        break;

    case wmf_E_EOF:
#ifdef DEBUG
        fprintf(stderr, "%s returned with wmf_E_EOF.\n", str);
#endif
        status = 1;
        break;

    case wmf_E_DeviceError:
#ifdef DEBUG
        fprintf(stderr, "%s returned with wmf_E_DeviceError.\n", str);
#endif
        status = 1;
        break;

    case wmf_E_Glitch:
#ifdef DEBUG
        fprintf(stderr, "%s returned with wmf_E_Glitch.\n", str);
#endif
        status = 1;

    case wmf_E_Assert:
#ifdef DEBUG
        fprintf(stderr, "%s returned with wmf_E_Assert.\n", str);
#endif
        status = 1;
        break;

    default:
#ifdef DEBUG
        fprintf(stderr, "%s returned unexpected value.\n", str);
#endif
        status = 1;
        break;
    }

    return (status);
}

int wmf2svg_draw(char *content, size_t size, float wmf_width, float wmf_height,
                 char **out, size_t *out_length) {
    int status = 0;

    unsigned long flags;

    FILE *out_f;
    out_f = open_memstream(out, out_length);

    ImageContext IC;

    wmf_error_t err;

    wmf_svg_t *ddata = 0;

    wmfAPI *API = 0;
    wmfD_Rect bbox;

    wmfAPI_Options api_options;

    flags = 0;

    flags |= WMF_OPT_FUNCTION;
    flags |= WMF_OPT_IGNORE_NONFATAL;

    api_options.function = wmf_svg_function;

    err = wmf_api_create(&API, flags, &api_options);
    status = explicit_wmf_error("wmf_api", err);

    wmfFunctionReference *FR = (wmfFunctionReference *)API->function_reference;

    FR->device_begin = or_wmf_svg_device_begin;
    FR->device_end = or_wmf_svg_device_end;

    if (status) {
        if (API)
            wmf_api_destroy(API);
        return (status);
    }

    err = wmf_mem_open(API, (unsigned char *)content, (long)size);
    status = explicit_wmf_error("open", err);

    if (status) {
        wmf_api_destroy(API);
        return (status);
    }

    err = wmf_scan(API, 0, &bbox);
    status = explicit_wmf_error("scan", err);

    if (status) {
        wmf_api_destroy(API);
        return (status);
    }

    ddata = WMF_SVG_GetData(API);

    float width;
    float height;
    wmf_size(API, &width, &height);

    if ((width <= 0) || (height <= 0)) {
#ifdef DEBUG
        fprintf(stderr, "Bad image size - but this error shouldn't occur...\n");
#endif
        status = 1;
        wmf_api_destroy(API);
        return (status);
    }

    // ddata->type = wmf_gd_jpeg;

    // ddata->flags |= WMF_SVG_OUTPUT_FILE;
    ddata->out = wmf_stream_create(API, out_f);

    ddata->bbox = bbox;
    ddata->width = wmf_width;
    ddata->height = wmf_height;
    ddata->flags |= WMF_SVG_INLINE_IMAGES;

    wmfD_Rect d_r;
    if (status == 0) {
        err = wmf_play(API, 0, &d_r);
        status = explicit_wmf_error("play", err);
    }

    fclose(out_f);
    wmf_api_destroy(API);

#ifdef DEBUG
    printf("%d, %d\n", err, status);
#endif
    return (status);
}

int Visio2Svg::visio2svg(std::string &in,
                         std::unordered_map<std::string, std::string> &out,
                         double scaling, int mode) {
    librevenge::RVNGStringStream input((const unsigned char *)in.c_str(),
                                       in.size());

    // check document type
    if (!libvisio::VisioDocument::isSupported(&input)) {
#ifdef DEBUG
        std::cerr << "ERROR: Unsupported file format (unsupported version) or "
                     "file is encrypted!"
                  << std::endl;
#endif
        return 1;
    }

    // Reset stream position after isSupported check
    input.seek(0, librevenge::RVNG_SEEK_SET);

    int ret = 0;

    // Recover Titles of each sheets
    librevenge::RVNGStringVector output_names;
    visio2svg::TitleGenerator generator_names(output_names);
    if (mode == VISIOVSS) {
        ret = libvisio::VisioDocument::parseStencils(&input, &generator_names);
    } else {
        ret = libvisio::VisioDocument::parse(&input, &generator_names);
    }

    if (!ret || output_names.empty()) {
#ifdef DEBUG
        std::cerr << "ERROR: Failed to recover sheets titles failed!"
                  << std::endl;
#endif
        return 1;
    }

    // Reset stream position before second parse
    input.seek(0, librevenge::RVNG_SEEK_SET);

    // Convert vss/vsd to SVG
    librevenge::RVNGStringVector output;
    librevenge::RVNGSVGDrawingGenerator generator(output, NULL);
    if (mode == VISIOVSS) {
        ret = libvisio::VisioDocument::parseStencils(&input, &generator);
    } else {
        ret = libvisio::VisioDocument::parse(&input, &generator);
    }
    if (!ret || output.empty()) {
#ifdef DEBUG
        std::cerr << "ERROR: SVG Generation failed!" << std::endl;
#endif
        return 1;
    }
    ret = 0;

    // Post Treatment loop and construction of the output hash table
    for (unsigned k = 0; k < output.size(); ++k) {
        char *post_treated = NULL;
        // Convert <image> tag containing emf blobs
        // and resize image
        ret |= postTreatement(&output[k], &output_names[k], &post_treated,
                              scaling);
        std::string title;
        if (output_names[k].empty()) {
            title = "no_title_" + std::to_string(k);
        } else {
            title = output_names[k].cstr();
        }
        std::pair<std::string, std::string> item(title,
                                                 std::string(post_treated));
        // output[k].cstr());
        free(post_treated);
        out.insert(item);
    }
    return ret;
}

// base64 decoder
// just copy paste from
// https://en.wikibooks.org/wiki/Algorithm_Implementation/Miscellaneous/Base64
static const unsigned char d[] = {
    66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 64, 66, 66, 66, 66, 66, 66, 66, 66,
    66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66,
    66, 66, 66, 66, 66, 62, 66, 66, 66, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60,
    61, 66, 66, 66, 65, 66, 66, 66, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10,
    11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 66, 66, 66, 66,
    66, 66, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42,
    43, 44, 45, 46, 47, 48, 49, 50, 51, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66,
    66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66,
    66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66,
    66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66,
    66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66,
    66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66,
    66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66,
    66, 66, 66, 66, 66, 66, 66, 66, 66};
int base64decode(char *in, size_t inLen, unsigned char *out, size_t *outLen) {
    char *end = in + inLen;
    char iter = 0;
    size_t buf = 0, len = 0;

    while (in < end) {
        unsigned char c = d[*in++];

        switch (c) {
        case WHITESPACE:
            continue; /* skip whitespace */
        case INVALID:
            return 1; /* invalid input, return error */
        case EQUALS:  /* pad character, end of data */
            in = end;
            continue;
        default:
            buf = buf << 6 | c;
            iter++; // increment the number of iteration
            /* If the buffer is full, split it into bytes */
            if (iter == 4) {
                if ((len += 3) > *outLen)
                    return 1; /* buffer overflow */
                *(out++) = (buf >> 16) & 255;
                *(out++) = (buf >> 8) & 255;
                *(out++) = buf & 255;
                buf = 0;
                iter = 0;
            }
        }
    }

    if (iter == 3) {
        if ((len += 2) > *outLen)
            return 1; /* buffer overflow */
        *(out++) = (buf >> 10) & 255;
        *(out++) = (buf >> 2) & 255;
    } else if (iter == 2) {
        if (++len > *outLen)
            return 1; /* buffer overflow */
        *(out++) = (buf >> 4) & 255;
    }

    *outLen = len; /* modify to reflect the actual output size */
    return 0;
}

// Recursive convert.
// Parse each node of the svg generated by librevenge/libvisio.
// If it encounters an <image> node, it checks if it's an emf blob
// and replace it with the result of emf2svg put inside a <g> node
// with proper translate() to get proper position.
// ===========================================================================
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

// NOTE: deliberately NOT std::min/std::max. On macOS this file pulls in
// memstream.c (see the #ifdef DARWIN block near the top), which defines
// old-style preprocessor macros named `min`/`max`. Those do blind textual
// substitution, so even a qualified call like std::min(...) gets mangled
// into invalid code (the `min` token inside `std::min` gets replaced
// regardless of the `std::` prefix). Using differently-named helpers here
// sidesteps that entirely instead of relying on #undef, which could have
// side effects on other code in this file that intentionally uses the
// bare min/max macros.
static inline double d_min(double a, double b) { return (a < b) ? a : b; }
static inline double d_max(double a, double b) { return (a > b) ? a : b; }

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
        // args may be comma- and/or whitespace-separated. Manual loop
        // instead of std::replace -- see the d_min/d_max note above for
        // why this file avoids <algorithm> call sites where possible.
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == ',')
                args[i] = ' ';
        }
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
        double ix0 = d_max(ox0, vx), iy0 = d_max(oy0, vy);
        double ix1 = d_min(ox1, vx + vw), iy1 = d_min(oy1, vy + vh);
        double iw = d_max(0.0, ix1 - ix0), ih = d_max(0.0, iy1 - iy0);
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


int convert_iterator(xmlNode *a_node, xmlDocPtr root_doc) {
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
                    wrapped[wrapped_len] = '\0';
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
}

int Visio2Svg::scale_title(xmlNode **root, xmlDocPtr *doc, double scaling,
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
}

int Visio2Svg::postTreatement(const librevenge::RVNGString *in,
                              const librevenge::RVNGString *name, char **out,
                              double scaling) {
    xmlDocPtr doc;
    xmlNode *root_element = NULL;
    int ret = 0;
// parse svg/xml generated by librevenge/libvisio with libxml2
#ifdef DEBUG
    std::cerr << "Converting: " << name->cstr() << std::endl;
#endif
    doc = xmlReadMemory(in->cstr(), in->size(), name->cstr(), NULL,
                        XML_PARSE_RECOVER | XML_PARSE_NOBLANKS |
                            XML_PARSE_NONET | XML_PARSE_HUGE);
    root_element = xmlDocGetRootElement(doc);
    xmlNodePtr comment = xmlNewDocComment(
        doc, (const unsigned char *)"converted by libvisio2svg");
    xmlAddChild(root_element, comment);
    // convert blobs (wmf, emf, ...)
    ret |= convert_iterator(root_element, doc);
    scale_title(&root_element, &doc, scaling, (const xmlChar *)name->cstr(),
                name->size());

    xmlBufferPtr nodeBuffer = xmlBufferCreate();
    xmlNodeDump(nodeBuffer, doc, root_element, 0, 1);
    // Dump the generated svg to out
    *out = strdup((char *)xmlBufferContent(nodeBuffer));
    // free some memory
    xmlFreeDoc(doc);
    xmlBufferFree(nodeBuffer);
    xmlCleanupParser();
    return ret;
}
}

/* vim:set shiftwidth=4 softtabstop=4 noexpandtab: */
