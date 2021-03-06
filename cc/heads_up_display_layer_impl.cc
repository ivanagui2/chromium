// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/heads_up_display_layer_impl.h"

#include "base/stringprintf.h"
#include "base/strings/string_split.h"
#include "cc/debug_colors.h"
#include "cc/debug_rect_history.h"
#include "cc/frame_rate_counter.h"
#include "cc/layer_tree_impl.h"
#include "cc/memory_history.h"
#include "cc/paint_time_counter.h"
#include "cc/quad_sink.h"
#include "cc/renderer.h"
#include "cc/texture_draw_quad.h"
#include "cc/tile_manager.h"
#include "skia/ext/platform_canvas.h"
#include "skia/ext/platform_canvas.h"
#include "third_party/khronos/GLES2/gl2.h"
#include "third_party/khronos/GLES2/gl2ext.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkFontHost.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include "third_party/skia/include/effects/SkColorMatrixFilter.h"
#include "ui/gfx/point.h"
#include "ui/gfx/size.h"

namespace cc {

static inline SkPaint createPaint()
{
    SkPaint paint;
#if (SK_R32_SHIFT || SK_B32_SHIFT != 16)
    // The SkCanvas is in RGBA but the shader is expecting BGRA, so we need to
    // swizzle our colors when drawing to the SkCanvas.
    SkColorMatrix swizzleMatrix;
    for (int i = 0; i < 20; ++i)
        swizzleMatrix.fMat[i] = 0;
    swizzleMatrix.fMat[0 + 5 * 2] = 1;
    swizzleMatrix.fMat[1 + 5 * 1] = 1;
    swizzleMatrix.fMat[2 + 5 * 0] = 1;
    swizzleMatrix.fMat[3 + 5 * 3] = 1;

    skia::RefPtr<SkColorMatrixFilter> filter =
        skia::AdoptRef(new SkColorMatrixFilter(swizzleMatrix));
    paint.setColorFilter(filter.get());
#endif
    return paint;
}

HeadsUpDisplayLayerImpl::Graph::Graph(double indicatorValue, double startUpperBound)
    : value(0)
    , min(0)
    , max(0)
    , currentUpperBound(startUpperBound)
    , defaultUpperBound(startUpperBound)
    , indicator(indicatorValue)
{
}

double HeadsUpDisplayLayerImpl::Graph::updateUpperBound()
{
    double targetUpperBound = std::max(max, defaultUpperBound);
    currentUpperBound += (targetUpperBound - currentUpperBound) * 0.5;
    return currentUpperBound;
}

HeadsUpDisplayLayerImpl::HeadsUpDisplayLayerImpl(LayerTreeImpl* treeImpl, int id)
    : LayerImpl(treeImpl, id)
    , m_fpsGraph(60.0, 80.0)
    , m_paintTimeGraph(16.0, 48.0)
    , m_typeface(skia::AdoptRef(SkFontHost::CreateTypeface(NULL, "monospace", SkTypeface::kBold)))
{
}

HeadsUpDisplayLayerImpl::~HeadsUpDisplayLayerImpl()
{
}

scoped_ptr<LayerImpl> HeadsUpDisplayLayerImpl::createLayerImpl(LayerTreeImpl* treeImpl)
{
    return HeadsUpDisplayLayerImpl::create(treeImpl, id()).PassAs<LayerImpl>();
}

void HeadsUpDisplayLayerImpl::willDraw(ResourceProvider* resourceProvider)
{
    LayerImpl::willDraw(resourceProvider);

    if (!m_hudTexture)
        m_hudTexture = ScopedResource::create(resourceProvider);

    // TODO(danakj): Scale the HUD by deviceScale to make it more friendly under high DPI.

    // TODO(danakj): The HUD could swap between two textures instead of creating a texture every frame in ubercompositor.
    if (m_hudTexture->size() != bounds() || resourceProvider->InUseByConsumer(m_hudTexture->id()))
        m_hudTexture->Free();

    if (!m_hudTexture->id()) {
        m_hudTexture->Allocate(bounds(), GL_RGBA, ResourceProvider::TextureUsageAny);
        // TODO(epenner): This texture was being used before SetPixels was called,
        // which is now not allowed (it's an uninitialized read). This should be fixed
        // and this allocateForTesting() removed.
        // http://crbug.com/166784
        resourceProvider->AllocateForTesting(m_hudTexture->id());
    }
}

void HeadsUpDisplayLayerImpl::appendQuads(QuadSink& quadSink, AppendQuadsData& appendQuadsData)
{
    if (!m_hudTexture->id())
        return;

    SharedQuadState* sharedQuadState = quadSink.useSharedQuadState(createSharedQuadState());

    gfx::Rect quadRect(gfx::Point(), bounds());
    gfx::Rect opaqueRect(contentsOpaque() ? quadRect : gfx::Rect());
    bool premultipliedAlpha = true;
    gfx::PointF uv_top_left(0.f, 0.f);
    gfx::PointF uv_bottom_right(1.f, 1.f);
    const float vertex_opacity[] = {1.f, 1.f, 1.f, 1.f};
    bool flipped = false;
    scoped_ptr<TextureDrawQuad> quad = TextureDrawQuad::Create();
    quad->SetNew(sharedQuadState, quadRect, opaqueRect, m_hudTexture->id(), premultipliedAlpha, uv_top_left, uv_bottom_right, vertex_opacity, flipped);
    quadSink.append(quad.PassAs<DrawQuad>(), appendQuadsData);
}

void HeadsUpDisplayLayerImpl::updateHudTexture(ResourceProvider* resourceProvider)
{
    if (!m_hudTexture->id())
        return;

    SkISize canvasSize;
    if (m_hudCanvas)
        canvasSize = m_hudCanvas->getDeviceSize();
    else
        canvasSize.set(0, 0);

    if (canvasSize.fWidth != bounds().width() || canvasSize.fHeight != bounds().height() || !m_hudCanvas)
        m_hudCanvas = make_scoped_ptr(skia::CreateBitmapCanvas(bounds().width(), bounds().height(), false /* opaque */));

    updateHudContents();

    m_hudCanvas->clear(SkColorSetARGB(0, 0, 0, 0));
    drawHudContents(m_hudCanvas.get());

    const SkBitmap* bitmap = &m_hudCanvas->getDevice()->accessBitmap(false);
    SkAutoLockPixels locker(*bitmap);

    gfx::Rect layerRect(gfx::Point(), bounds());
    DCHECK(bitmap->config() == SkBitmap::kARGB_8888_Config);
    resourceProvider->SetPixels(m_hudTexture->id(), static_cast<const uint8_t*>(bitmap->getPixels()), layerRect, layerRect, gfx::Vector2d());
}

void HeadsUpDisplayLayerImpl::didDraw(ResourceProvider* resourceProvider)
{
    LayerImpl::didDraw(resourceProvider);

    if (!m_hudTexture->id())
        return;

    // FIXME: the following assert will not be true when sending resources to a
    // parent compositor. We will probably need to hold on to m_hudTexture for
    // longer, and have several HUD textures in the pipeline.
    DCHECK(!resourceProvider->InUseByConsumer(m_hudTexture->id()));
}

void HeadsUpDisplayLayerImpl::didLoseOutputSurface()
{
    m_hudTexture.reset();
}

bool HeadsUpDisplayLayerImpl::layerIsAlwaysDamaged() const
{
    return true;
}

void HeadsUpDisplayLayerImpl::updateHudContents()
{
    const LayerTreeDebugState& debugState = layerTreeImpl()->debug_state();

    // Don't update numbers every frame so text is readable.
    base::TimeTicks now = layerTreeImpl()->CurrentFrameTime();
    if (base::TimeDelta(now - m_timeOfLastGraphUpdate).InSecondsF() > 0.25f) {
        m_timeOfLastGraphUpdate = now;

        if (debugState.showFPSCounter) {
            FrameRateCounter* fpsCounter = layerTreeImpl()->frame_rate_counter();
            m_fpsGraph.value = fpsCounter->getAverageFPS();
            fpsCounter->getMinAndMaxFPS(m_fpsGraph.min, m_fpsGraph.max);
        }

        if (debugState.continuousPainting) {
            PaintTimeCounter* paintTimeCounter = layerTreeImpl()->paint_time_counter();
            base::TimeDelta latest, min, max;

            if (paintTimeCounter->End())
                latest = paintTimeCounter->End()->total_time();
            paintTimeCounter->GetMinAndMaxPaintTime(&min, &max);

            m_paintTimeGraph.value = latest.InMillisecondsF();
            m_paintTimeGraph.min = min.InMillisecondsF();
            m_paintTimeGraph.max = max.InMillisecondsF();
        }

        if (debugState.showMemoryStats()) {
            MemoryHistory* memoryHistory = layerTreeImpl()->memory_history();
            if (memoryHistory->End())
                m_memoryEntry = **memoryHistory->End();
            else
                m_memoryEntry = MemoryHistory::Entry();
        }
    }

    m_fpsGraph.updateUpperBound();
    m_paintTimeGraph.updateUpperBound();
}

void HeadsUpDisplayLayerImpl::drawHudContents(SkCanvas* canvas) const
{
    const LayerTreeDebugState& debugState = layerTreeImpl()->debug_state();

    if (debugState.showHudRects())
        drawDebugRects(canvas, layerTreeImpl()->debug_rect_history());

    if (debugState.showPlatformLayerTree)
        drawPlatformLayerTree(canvas);

    SkRect area = SkRect::MakeEmpty();
    if (debugState.continuousPainting)
        area = drawPaintTimeDisplay(canvas, layerTreeImpl()->paint_time_counter(), 0, 0);
    // Don't show the FPS display when continuous painting is enabled, because it would show misleading numbers.
    else if (debugState.showFPSCounter)
        area = drawFPSDisplay(canvas, layerTreeImpl()->frame_rate_counter(), 0, 0);

    if (debugState.showMemoryStats())
        drawMemoryDisplay(canvas, 0, area.bottom(), SkMaxScalar(area.width(), 150));
}

void HeadsUpDisplayLayerImpl::drawText(SkCanvas* canvas, SkPaint* paint, const std::string& text, SkPaint::Align align, int size, int x, int y) const
{
    const bool antiAlias = paint->isAntiAlias();
    paint->setAntiAlias(true);

    paint->setTextSize(size);
    paint->setTextAlign(align);
    paint->setTypeface(m_typeface.get());
    canvas->drawText(text.c_str(), text.length(), x, y, *paint);

    paint->setAntiAlias(antiAlias);
}

void HeadsUpDisplayLayerImpl::drawText(SkCanvas* canvas, SkPaint* paint, const std::string& text, SkPaint::Align align, int size, const SkPoint& pos) const
{
    drawText(canvas, paint, text, align, size, pos.x(), pos.y());
}

void HeadsUpDisplayLayerImpl::drawGraphBackground(SkCanvas* canvas, SkPaint* paint, const SkRect& bounds) const
{
    paint->setColor(DebugColors::HUDBackgroundColor());
    canvas->drawRect(bounds, *paint);
}

void HeadsUpDisplayLayerImpl::drawGraphLines(SkCanvas* canvas, SkPaint* paint, const SkRect& bounds, const Graph& graph) const
{
    // Draw top and bottom line.
    paint->setColor(DebugColors::HUDSeparatorLineColor());
    canvas->drawLine(bounds.left(), bounds.top() - 1, bounds.right(), bounds.top() - 1, *paint);
    canvas->drawLine(bounds.left(), bounds.bottom(), bounds.right(), bounds.bottom(), *paint);

    // Draw indicator line (additive blend mode to increase contrast when drawn on top of graph).
    paint->setColor(DebugColors::HUDIndicatorLineColor());
    paint->setXfermodeMode(SkXfermode::kPlus_Mode);
    const double indicatorTop = bounds.height() * (1 - graph.indicator / graph.currentUpperBound) - 1;
    canvas->drawLine(bounds.left(), bounds.top() + indicatorTop, bounds.right(), bounds.top() + indicatorTop, *paint);
    paint->setXfermode(NULL);
}

void HeadsUpDisplayLayerImpl::drawPlatformLayerTree(SkCanvas* canvas) const
{
    const int fontHeight = 14;
    SkPaint paint = createPaint();
    drawGraphBackground(canvas, &paint, SkRect::MakeXYWH(0, 0, bounds().width(), bounds().height()));

    std::string layerTree = layerTreeImpl()->layer_tree_as_text();
    std::vector<std::string> lines;
    base::SplitString(layerTree, '\n', &lines);

    paint.setColor(DebugColors::PlatformLayerTreeTextColor());
    for (size_t i = 0; i < lines.size() && static_cast<int>(2 + i * fontHeight) < bounds().height(); ++i) {
        drawText(canvas, &paint, lines[i], SkPaint::kLeft_Align, fontHeight, 2, 2 + (i + 1) * fontHeight);
    }
}

SkRect HeadsUpDisplayLayerImpl::drawFPSDisplay(SkCanvas* canvas, const FrameRateCounter* fpsCounter, int right, int top) const
{
    const int padding = 4;
    const int gap = 6;

    const int fontHeight = 15;

    const int graphWidth = fpsCounter->timeStampHistorySize() - 2;
    const int graphHeight = 40;

    const int histogramWidth = 37;

    const int width = graphWidth + histogramWidth + 4 * padding;
    const int height = fontHeight + graphHeight + 4 * padding + 2;
    const int left = bounds().width() - width - right;

    const SkRect area = SkRect::MakeXYWH(left, top, width, height);

    SkPaint paint = createPaint();
    drawGraphBackground(canvas, &paint, area);

    SkRect textBounds = SkRect::MakeXYWH(left + padding, top + padding, graphWidth + histogramWidth + gap + 2, fontHeight);
    SkRect graphBounds = SkRect::MakeXYWH(left + padding, textBounds.bottom() + 2 * padding, graphWidth, graphHeight);
    SkRect histogramBounds = SkRect::MakeXYWH(graphBounds.right() + gap, graphBounds.top(), histogramWidth, graphHeight);

    const std::string valueText = base::StringPrintf("FPS:%5.1f", m_fpsGraph.value);
    const std::string minMaxText = base::StringPrintf("%.0f-%.0f", m_fpsGraph.min, m_fpsGraph.max);

    paint.setColor(DebugColors::FPSDisplayTextAndGraphColor());
    drawText(canvas, &paint, valueText, SkPaint::kLeft_Align, fontHeight, textBounds.left(), textBounds.bottom());
    drawText(canvas, &paint, minMaxText, SkPaint::kRight_Align, fontHeight, textBounds.right(), textBounds.bottom());

    drawGraphLines(canvas, &paint, graphBounds, m_fpsGraph);

    // Collect graph and histogram data.
    SkPath path;

    const int histogramSize = 20;
    double histogram[histogramSize] = {0};
    double maxBucketValue = 0;

    for (FrameRateCounter::RingBufferType::Iterator it = --fpsCounter->end(); it; --it) {
        base::TimeDelta delta = fpsCounter->recentFrameInterval(it.index() + 1);

        // Skip this particular instantaneous frame rate if it is not likely to have been valid.
        if (!fpsCounter->isBadFrameInterval(delta)) {

            double fps = 1.0 / delta.InSecondsF();

            // Clamp the FPS to the range we want to plot visually.
            double p = fps / m_fpsGraph.currentUpperBound;
            if (p > 1)
                p = 1;

            // Plot this data point.
            SkPoint cur = SkPoint::Make(graphBounds.left() + it.index(), graphBounds.bottom() - p * graphBounds.height());
            if (path.isEmpty())
                path.moveTo(cur);
            else
                path.lineTo(cur);

            // Use the fps value to find the right bucket in the histogram.
            int bucketIndex = floor(p * (histogramSize - 1));

            // Add the delta time to take the time spent at that fps rate into account.
            histogram[bucketIndex] += delta.InSecondsF();
            maxBucketValue = std::max(histogram[bucketIndex], maxBucketValue);
        }
    }

    // Draw FPS histogram.
    paint.setColor(DebugColors::HUDSeparatorLineColor());
    canvas->drawLine(histogramBounds.left() - 1, histogramBounds.top() - 1, histogramBounds.left() - 1, histogramBounds.bottom() + 1, paint);
    canvas->drawLine(histogramBounds.right() + 1, histogramBounds.top() - 1, histogramBounds.right() + 1, histogramBounds.bottom() + 1, paint);

    paint.setColor(DebugColors::FPSDisplayTextAndGraphColor());
    const double barHeight = histogramBounds.height() / histogramSize;

    for (int i = histogramSize - 1; i >= 0; --i) {
        if (histogram[i] > 0) {
            double barWidth = histogram[i] / maxBucketValue * histogramBounds.width();
            canvas->drawRect(SkRect::MakeXYWH(histogramBounds.left(), histogramBounds.bottom() - (i + 1) * barHeight, barWidth, 1), paint);
        }
    }

    // Draw FPS graph.
    paint.setAntiAlias(true);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(1);
    canvas->drawPath(path, paint);

    return area;
}

SkRect HeadsUpDisplayLayerImpl::drawMemoryDisplay(SkCanvas* canvas, int right, int top, int width) const
{
    if (!m_memoryEntry.bytes_total())
        return SkRect::MakeEmpty();

    const int padding = 4;
    const int fontHeight = 13;

    const int height = 3 * fontHeight + 4 * padding;
    const int left = bounds().width() - width - right;
    const SkRect area = SkRect::MakeXYWH(left, top, width, height);

    const double megabyte = static_cast<double>(1024*1024);

    SkPaint paint = createPaint();
    drawGraphBackground(canvas, &paint, area);

    SkPoint titlePos = SkPoint::Make(left + padding, top + fontHeight);
    SkPoint stat1Pos = SkPoint::Make(left + width - padding - 1, top + padding + 2 * fontHeight);
    SkPoint stat2Pos = SkPoint::Make(left + width - padding - 1, top + 2 * padding + 3 * fontHeight);

    paint.setColor(DebugColors::MemoryDisplayTextColor());
    drawText(canvas, &paint, "GPU memory", SkPaint::kLeft_Align, fontHeight, titlePos);

    std::string text = base::StringPrintf(
        "%6.1f MB used",
        (m_memoryEntry.bytes_unreleasable + m_memoryEntry.bytes_allocated) / megabyte);
    drawText(canvas, &paint, text, SkPaint::kRight_Align, fontHeight, stat1Pos);

    if (m_memoryEntry.bytes_over) {
      paint.setColor(SK_ColorRED);
      text = base::StringPrintf("%6.1f MB over",
                                m_memoryEntry.bytes_over / megabyte);
    } else {
      text = base::StringPrintf("%6.1f MB max ",
                                m_memoryEntry.total_budget_in_bytes / megabyte);
    }
    drawText(canvas, &paint, text, SkPaint::kRight_Align, fontHeight, stat2Pos);

    return area;
}

SkRect HeadsUpDisplayLayerImpl::drawPaintTimeDisplay(SkCanvas* canvas, const PaintTimeCounter* paintTimeCounter, int right, int top) const
{
    const int padding = 4;
    const int fontHeight = 15;

    const int graphWidth = paintTimeCounter->HistorySize();
    const int graphHeight = 40;

    const int width = graphWidth + 2 * padding;
    const int height = fontHeight + graphHeight + 4 * padding + 2 + fontHeight + padding;
    const int left = bounds().width() - width - right;

    const SkRect area = SkRect::MakeXYWH(left, top, width, height);

    SkPaint paint = createPaint();
    drawGraphBackground(canvas, &paint, area);

    SkRect textBounds = SkRect::MakeXYWH(left + padding, top + padding, graphWidth, fontHeight);
    SkRect textBounds2 = SkRect::MakeXYWH(left + padding, textBounds.bottom() + padding, graphWidth, fontHeight);
    SkRect graphBounds = SkRect::MakeXYWH(left + padding, textBounds2.bottom() + 2 * padding, graphWidth, graphHeight);

    const std::string valueText = base::StringPrintf("%.1f", m_paintTimeGraph.value);
    const std::string minMaxText = base::StringPrintf("%.1f-%.1f", m_paintTimeGraph.min, m_paintTimeGraph.max);

    paint.setColor(DebugColors::PaintTimeDisplayTextAndGraphColor());
    drawText(canvas, &paint, "Page paint time (ms)", SkPaint::kLeft_Align, fontHeight, textBounds.left(), textBounds.bottom());
    drawText(canvas, &paint, valueText, SkPaint::kLeft_Align, fontHeight, textBounds2.left(), textBounds2.bottom());
    drawText(canvas, &paint, minMaxText, SkPaint::kRight_Align, fontHeight, textBounds2.right(), textBounds2.bottom());

    paint.setColor(DebugColors::PaintTimeDisplayTextAndGraphColor());
    for (PaintTimeCounter::RingBufferType::Iterator it = paintTimeCounter->End(); it; --it) {
        double pt = it->total_time().InMillisecondsF();

        if (pt == 0.0)
            continue;

        double p = pt / m_paintTimeGraph.currentUpperBound;
        if (p > 1)
            p = 1;

        canvas->drawRect(SkRect::MakeXYWH(graphBounds.left() + it.index(), graphBounds.bottom() - p * graphBounds.height(), 1, p * graphBounds.height()), paint);
    }

    drawGraphLines(canvas, &paint, graphBounds, m_paintTimeGraph);

    return area;
}

void HeadsUpDisplayLayerImpl::drawDebugRects(SkCanvas* canvas, DebugRectHistory* debugRectHistory) const
{
    const std::vector<DebugRect>& debugRects = debugRectHistory->debugRects();
    float rectScale = 1 / layerTreeImpl()->device_scale_factor();
    SkPaint paint = createPaint();

    canvas->save();
    canvas->scale(rectScale, rectScale);

    for (size_t i = 0; i < debugRects.size(); ++i) {
        SkColor strokeColor = 0;
        SkColor fillColor = 0;
        float strokeWidth = 0;

        switch (debugRects[i].type) {
        case PaintRectType:
            strokeColor = DebugColors::PaintRectBorderColor();
            fillColor = DebugColors::PaintRectFillColor();
            strokeWidth = DebugColors::PaintRectBorderWidth(layerTreeImpl());
            break;
        case PropertyChangedRectType:
            strokeColor = DebugColors::PropertyChangedRectBorderColor();
            fillColor = DebugColors::PropertyChangedRectFillColor();
            strokeWidth = DebugColors::PropertyChangedRectBorderWidth(layerTreeImpl());
            break;
        case SurfaceDamageRectType:
            strokeColor = DebugColors::SurfaceDamageRectBorderColor();
            fillColor = DebugColors::SurfaceDamageRectFillColor();
            strokeWidth = DebugColors::SurfaceDamageRectBorderWidth(layerTreeImpl());
            break;
        case ReplicaScreenSpaceRectType:
            strokeColor = DebugColors::ScreenSpaceSurfaceReplicaRectBorderColor();
            fillColor = DebugColors::ScreenSpaceSurfaceReplicaRectFillColor();
            strokeWidth = DebugColors::ScreenSpaceSurfaceReplicaRectBorderWidth(layerTreeImpl());
            break;
        case ScreenSpaceRectType:
            strokeColor = DebugColors::ScreenSpaceLayerRectBorderColor();
            fillColor = DebugColors::ScreenSpaceLayerRectFillColor();
            strokeWidth = DebugColors::ScreenSpaceLayerRectBorderWidth(layerTreeImpl());
            break;
        case OccludingRectType:
            strokeColor = DebugColors::OccludingRectBorderColor();
            fillColor = DebugColors::OccludingRectFillColor();
            strokeWidth = DebugColors::OccludingRectBorderWidth(layerTreeImpl());
            break;
        case NonOccludingRectType:
            strokeColor = DebugColors::NonOccludingRectBorderColor();
            fillColor = DebugColors::NonOccludingRectFillColor();
            strokeWidth = DebugColors::NonOccludingRectBorderWidth(layerTreeImpl());
            break;
        }

        const gfx::RectF& rect = debugRects[i].rect;
        SkRect skRect = SkRect::MakeXYWH(rect.x(), rect.y(), rect.width(), rect.height());
        paint.setColor(fillColor);
        paint.setStyle(SkPaint::kFill_Style);
        canvas->drawRect(skRect, paint);

        paint.setColor(strokeColor);
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(SkFloatToScalar(strokeWidth));
        canvas->drawRect(skRect, paint);
    }

    canvas->restore();
}

const char* HeadsUpDisplayLayerImpl::layerTypeAsString() const
{
    return "HeadsUpDisplayLayer";
}

}  // namespace cc
