// Inline SVG (SVG 2): the shapes of an <svg> element in an HTML page, turned into paths when the page is laid out and
// drawn by the GPU painter with Direct2D geometry.
//
// What is drawn: path, rect, circle, ellipse, line, polyline and polygon, inside g, a, nested svg and use (of any
// element or a symbol), each with its transform attribute; fill (a colour, currentColor or none), fill-rule,
// fill-opacity, stroke, stroke-width, stroke-opacity and opacity, from presentation attributes or CSS; viewBox with
// preserveAspectRatio. Not yet: text, images, gradients and patterns (such a fill draws nothing unless it names a
// fallback colour), clip paths, masks, markers, stroke line caps, joins and dashes.
#pragma once
#include <d2d1.h>
#include <wrl/client.h>

#include <vector>

#include "common.h"
#include "dom.h"

namespace tb {

// One shape: its outline in the coordinates of the svg's viewBox, and how it is painted.
struct SvgShape {
    enum Op : uint8_t { Move, Line, Cubic, Quad, Arc, Close };
    std::vector<Op> ops;
    std::vector<float> args;  // Move/Line: x y; Cubic: x1 y1 x2 y2 x y; Quad: x1 y1 x y; Arc: rx ry angle large sweep x y
    bool evenOdd = false;
    bool fill = true, stroke = false;
    Color fillColor{0, 0, 0, 1}, strokeColor{0, 0, 0, 1};
    float strokeWidth = 1;
    float m[6] = {1, 0, 0, 1, 0, 0};  // to viewBox coordinates: x' = a x + c y + e, y' = b x + d y + f
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;  // made by the painter the first time it is drawn
};

// An outermost <svg> element: its viewBox, how that is fitted into the element's box, and its shapes.
struct SvgImage {
    bool hasViewBox = false;
    float vb[4] = {0, 0, 0, 0};  // min-x min-y width height
    uint8_t alignX = 1, alignY = 1;  // 0 min, 1 mid, 2 max
    bool slice = false, stretch = false;  // preserveAspectRatio: slice, or none
    size_t first = 0, count = 0;  // its shapes in the list they share
};

// The viewBox of an svg element, as width / height; 0 when it has none.
float svg_aspect(const Node* svg);
// Collects the shapes an svg element draws, appending them to `shapes`.
SvgImage svg_build(const Node* svg, const Document* doc, std::vector<SvgShape>& shapes);
// Draws an svg into the box x y w h, clipped to that box.
void svg_paint(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const SvgImage& img, std::vector<SvgShape>& shapes, float x, float y,
               float w, float h);

}  // namespace tb
