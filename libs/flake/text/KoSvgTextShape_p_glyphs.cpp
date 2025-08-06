/*
 *  SPDX-FileCopyrightText: 2017 Dmitry Kazakov <dimula73@gmail.com>
 *  SPDX-FileCopyrightText: 2022 Wolthera van Hövell tot Westerflier <griffinvalley@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KoSvgTextShape.h"
#include "KoSvgTextShape_p.h"

#include "KoFontLibraryResourceUtils.h"

#include <FlakeDebug.h>
#include <KoPathShape.h>

#include <kis_global.h>

#include <QPainterPath>
#include <QtMath>

#include <utility>
#include <variant>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_COLOR_H
#include FT_BITMAP_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H

#include <hb.h>
#include <hb-ft.h>

#include <raqm.h>


static QPainterPath convertFromFreeTypeOutline(FT_GlyphSlotRec *glyphSlot);
static QImage convertFromFreeTypeBitmap(FT_GlyphSlotRec *glyphSlot);

static QString glyphFormatToStr(const FT_Glyph_Format _v)
{
    const unsigned int v = _v;
    QString s;
    s += (v >> 24) & 0xFF;
    s += (v >> 16) & 0xFF;
    s += (v >> 8) & 0xFF;
    s += (v >> 0) & 0xFF;
    return s;
}

/**
 * @brief Embolden a glyph (synthesize bold) if the font does not have native
 * bold.
 *
 * @param ftface
 * @param charResult
 * @param x_advance Pointer to the X advance to be adjusted if needed.
 * @param y_advance Pointer to the Y advance to be adjusted if needed.
 */
static void
emboldenGlyphIfNeeded(const FT_Face ftface, const CharacterResult &charResult, int *x_advance, int *y_advance)
{
    constexpr int WEIGHT_SEMIBOLD = 600;
    if (charResult.fontWeight >= WEIGHT_SEMIBOLD) {
        // Simplest check: Bold fonts don't need to be embolden.
        if (ftface->style_flags & FT_STYLE_FLAG_BOLD) {
            return;
        }

        // Variable fnots also don't need to be embolden.
        if (FT_HAS_MULTIPLE_MASTERS(ftface)) {
            return;
        }

        // Some heavy weight classes don't cause FT_STYLE_FLAG_BOLD to be set,
        // so we have to check the OS/2 table for its weight class to be sure.
        if (const TT_OS2 *const os2Table = reinterpret_cast<TT_OS2 *>(FT_Get_Sfnt_Table(ftface, FT_SFNT_OS2));
            os2Table && os2Table->usWeightClass >= WEIGHT_SEMIBOLD) {
            return;
        }

        // This code is somewhat inspired by Firefox.
        FT_Pos strength =
            FT_MulFix(ftface->units_per_EM, ftface->size->metrics.y_scale) / 48;

        if (ftface->glyph->format == FT_GLYPH_FORMAT_BITMAP) {
            // This is similar to what FT_GlyphSlot_Embolden does.

            // Round down to full pixel.
            strength &= ~63;
            if (strength == 0) {
                // ... but it has to be at least one pixel.
                strength = 64;
            }

            FT_GlyphSlot_Own_Bitmap(ftface->glyph);

            // Embolden less vertically than horizontally. Especially if
            // strength is only 1px, don't embolden vertically at all.
            // Otherwise it makes the glyph way too heavy, especially for
            // CJK glyphs in small sizes.
            const FT_Pos strengthY = strength - 64;
            FT_Bitmap_Embolden(ftface->glyph->library, &ftface->glyph->bitmap, strength, strengthY);

            if (x_advance && *x_advance != 0) {
                *x_advance += strength;
            }
            if (y_advance && *y_advance != 0) {
                *y_advance -= strengthY;
            }
        } else {
            FT_Outline_Embolden(&ftface->glyph->outline, strength);

            if (x_advance && *x_advance != 0) {
                *x_advance += strength;
            }
            if (y_advance && *y_advance != 0) {
                *y_advance -= strength;
            }
        }
    }
}

/**
 * @brief Calculate the transformation matrices for an outline glyph, taking
 * synthesized italic into account.
 *
 * @param ftTF FT unit to 1/72 unit
 * @param currentGlyph
 * @param charResult
 * @param isHorizontal
 * @return std::pair<QTransform, QTransform> {outlineGlyphTf, glyphObliqueTf}
 */
static std::pair<QTransform, QTransform> calcOutlineGlyphTransform(const QTransform &ftTF,
                                                                   const raqm_glyph_t &currentGlyph,
                                                                   const CharacterResult &charResult,
                                                                   const bool isHorizontal)
{
    QTransform outlineGlyphTf = QTransform::fromTranslate(currentGlyph.x_offset, currentGlyph.y_offset);
    QTransform glyphObliqueTf;

    // Check whether we need to synthesize italic by shearing the glyph:
    if (charResult.fontStyle != QFont::StyleNormal && !(currentGlyph.ftface->style_flags & FT_STYLE_FLAG_ITALIC)) {
        // CSS Fonts Module Level 4, 2.4. Font style: the font-style property:
        // For `oblique`, "lack of an <angle> represents 14deg".
        constexpr double SLANT_14DEG = 0.24932800284318069162403993780486;
        if (isHorizontal) {
            glyphObliqueTf.shear(SLANT_14DEG, 0);
        } else {
            // For vertical mode, CSSWG says:
            // - Skew around the centre
            // - Right-side down and left-side up
            // https://github.com/w3c/csswg-drafts/issues/2869
            glyphObliqueTf.shear(0, -SLANT_14DEG);
        }
        outlineGlyphTf *= glyphObliqueTf;
    }
    outlineGlyphTf *= ftTF;
    return {outlineGlyphTf, glyphObliqueTf};
}

/**
 * @brief Helper class to load CPAL/COLR v0 color layers, functionally based
 * off the sample code in the freetype docs.
 */
class ColorLayersLoader
{
public:
    /**
     * @brief Construct a ColorLayersLoader object. The first color layer is
     * selected if there are any.
     *
     * @param face
     * @param baseGlyph
     */
    ColorLayersLoader(FT_Face face, FT_UInt baseGlyph)
        : m_face(face)
        , m_baseGlyph(baseGlyph)
    {
        const unsigned short paletteIndex = 0;
        if (FT_Palette_Select(m_face, paletteIndex, &m_palette) != 0) {
            m_palette = nullptr;
        }
        m_haveLayers = moveNext();
    }

    /**
     * @brief Check whether there are color layers to be loaded.
     */
    operator bool() const
    {
        return m_haveLayers && m_palette;
    }

    /**
     * @brief Load the current glyph layer.
     *
     * @param charResult
     * @param faceLoadFlags
     * @param x_advance Pointer to the X advance to be adjusted if needed.
     * @param y_advance Pointer to the Y advance to be adjusted if needed.
     * @return std::tuple<QPainterPath, QBrush, bool> {glyphOutline, layerColor, isForeGroundColor}
     */
    std::tuple<QPainterPath, QBrush, bool>
    layer(const CharacterResult &charResult, const FT_Int32 faceLoadFlags, int *x_advance, int *y_advance)
    {
        QBrush layerColor;
        bool isForeGroundColor = false;

        if (m_layerColorIndex == 0xFFFF) {
            layerColor = Qt::black;
            isForeGroundColor = true;
        } else {
            const FT_Color color = m_palette[m_layerColorIndex];
            layerColor = QColor(color.red, color.green, color.blue, color.alpha);
        }
        if (const FT_Error err = FT_Load_Glyph(m_face, m_layerGlyphIndex, faceLoadFlags)) {
            warnFlake << "Failed to load glyph, freetype error" << err;
            return {};
        }
        if (m_face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
            // Check whether we need to synthesize bold by emboldening the glyph:
            emboldenGlyphIfNeeded(m_face, charResult, x_advance, y_advance);

            const QPainterPath p = convertFromFreeTypeOutline(m_face->glyph);
            return {p, layerColor, isForeGroundColor};
        } else {
            warnFlake << "Unsupported glyph format" << glyphFormatToStr(m_face->glyph->format) << "in glyph layers";
            return {};
        }
    }

    /**
     * @brief Move to the next glyph layer.
     *
     * @return true if there are more layers.
     * @return false if there are no more layers.
     */
    bool moveNext()
    {
        m_haveLayers =
            FT_Get_Color_Glyph_Layer(m_face, m_baseGlyph, &m_layerGlyphIndex, &m_layerColorIndex, &m_iterator);
        return m_haveLayers;
    }

private:
    FT_UInt m_layerGlyphIndex{};
    FT_UInt m_layerColorIndex{};
    FT_LayerIterator m_iterator{};
    FT_Color *m_palette{};
    FT_Face m_face;
    FT_UInt m_baseGlyph;
    bool m_haveLayers;
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool KoSvgTextShape::Private::loadGlyph(const QTransform &ftTF,
                                        const QMap<int, KoSvgText::TabSizeInfo> &tabSizeInfo,
                                        const FT_Int32 faceLoadFlags,
                                        const bool isHorizontal,
                                        const int i,
                                        raqm_glyph_t &currentGlyph,
                                        QMap<int, int> &logicalToVisual,
                                        CharacterResult &charResult,
                                        QPointF &totalAdvanceFTFontCoordinates) const
{
    // Whenever the freetype docs talk about a 26.6 floating point unit, they
    // mean a 1/64 value.
    const qreal ftFontUnit = 64.0;
    const qreal ftFontUnitFactor = 1 / ftFontUnit;

    const int cluster = static_cast<int>(currentGlyph.cluster);

    QPointF spaceAdvance;
    if (tabSizeInfo.contains(cluster)) {
        FT_Load_Glyph(currentGlyph.ftface, FT_Get_Char_Index(currentGlyph.ftface, ' '), faceLoadFlags);
        spaceAdvance = QPointF(currentGlyph.ftface->glyph->advance.x, currentGlyph.ftface->glyph->advance.y);
    }

    /// The matrix for Italic (oblique) synthesis of outline glyphs, or for
    /// adjusting the bounding box of bitmap glyphs.
    QTransform glyphObliqueTf;

    /// The scaling factor for color bitmap glyphs, otherwise always 1.0
    qreal bitmapScale = 1.0;

    // Try to retrieve CPAL/COLR v0 color layers, this should be preferred over
    // other glyph formats. Doing this first also allows us to skip loading the
    // default outline glyph.
    if (ColorLayersLoader loader{currentGlyph.ftface, currentGlyph.index}) {
        Glyph::ColorLayers *colorGlyph = std::get_if<Glyph::ColorLayers>(&charResult.glyph);
        if (!colorGlyph) {
            if (!std::holds_alternative<std::monostate>(charResult.glyph)) {
                warnFlake << "Glyph contains other type than ColorLayers:" << charResult.glyph.index();
            }
            colorGlyph = &charResult.glyph.emplace<Glyph::ColorLayers>();
        }

        /// The combined offset * italic * ftTf transform for outline glyphs.
        QTransform outlineGlyphTf;

        // Calculate the transforms
        std::tie(outlineGlyphTf, glyphObliqueTf) =
            calcOutlineGlyphTransform(ftTF, currentGlyph, charResult, isHorizontal);

        const int orig_x_advance = currentGlyph.x_advance;
        const int orig_y_advance = currentGlyph.y_advance;
        int new_x_advance{};
        int new_y_advance{};
        do {
            new_x_advance = orig_x_advance;
            new_y_advance = orig_y_advance;
            QPainterPath p;
            QBrush layerColor;
            bool isForeGroundColor = false;
            std::tie(p, layerColor, isForeGroundColor) = loader.layer(charResult, faceLoadFlags, &new_x_advance, &new_y_advance);
            if (!p.isEmpty()) {
                p = outlineGlyphTf.map(p);
                if (charResult.visualIndex > -1) {
                    // This is for glyph clusters, i.e. complex emoji. Do it
                    // like how we handle unicode combining marks.
                    p = p.translated(charResult.advance);
                }
                colorGlyph->paths.append(p);
                colorGlyph->colors.append(layerColor);
                colorGlyph->replaceWithForeGroundColor.append(isForeGroundColor);
            }
        } while (loader.moveNext());
        currentGlyph.x_advance = new_x_advance;
        currentGlyph.y_advance = new_y_advance;
    } else {
        if (const FT_Error err = FT_Load_Glyph(currentGlyph.ftface, currentGlyph.index, faceLoadFlags)) {
            warnFlake << "Failed to load glyph, freetype error" << err;
            return false;
        }

        // Check whether we need to synthesize bold by emboldening the glyph:
        emboldenGlyphIfNeeded(currentGlyph.ftface, charResult, &currentGlyph.x_advance, &currentGlyph.y_advance);

        if (currentGlyph.ftface->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
        Glyph::Outline _discard; ///< Storage for discarded outline, must outlive outlineGlyph
        Glyph::Outline *outlineGlyph = std::get_if<Glyph::Outline>(&charResult.glyph);
        if (!outlineGlyph) {
            if (std::holds_alternative<Glyph::ColorLayers>(charResult.glyph)) {
                // Special case: possibly an empty glyph in the middle of a
                // combining color glyph, just discard the resulting path.
                outlineGlyph = &_discard;
            } else if (!std::holds_alternative<std::monostate>(charResult.glyph)) {
                warnFlake << "Glyph contains other type than Outline:" << charResult.glyph.index();
            }
            if (!outlineGlyph) {
                outlineGlyph = &charResult.glyph.emplace<Glyph::Outline>();
            }
        }

            /// The combined offset * italic * ftTf transform for outline glyphs.
            QTransform outlineGlyphTf;

            // Calculate the transforms
            std::tie(outlineGlyphTf, glyphObliqueTf) =
                calcOutlineGlyphTransform(ftTF, currentGlyph, charResult, isHorizontal);

            QPainterPath glyph = convertFromFreeTypeOutline(currentGlyph.ftface->glyph);
            glyph = outlineGlyphTf.map(glyph);

            if (charResult.visualIndex > -1) {
                // this is for glyph clusters, unicode combining marks are always
                // added. we could have these as seperate paths, but there's no real
                // purpose, and the svg standard prefers 'ligatures' to be treated
                // as a single glyph. It simplifies things for us in any case.
                outlineGlyph->path.addPath(glyph.translated(charResult.advance));
            } else {
                outlineGlyph->path = glyph;
            }
        } else {
            QTransform bitmapTf;

            if (currentGlyph.ftface->glyph->format == FT_GLYPH_FORMAT_BITMAP) {
                if (FT_HAS_COLOR(currentGlyph.ftface)) {
                    // This applies the transform for CBDT bitmaps (e.g. Noto
                    // Color Emoji) that was set in KoFontRegistry::configureFaces
                    FT_Matrix matrix;
                    FT_Vector delta;
                    FT_Get_Transform(currentGlyph.ftface, &matrix, &delta);
                    constexpr qreal FACTOR_16 = 1.0 / 65536.0;
                    bitmapTf.setMatrix(matrix.xx * FACTOR_16, matrix.xy * FACTOR_16, 0, matrix.yx * FACTOR_16, matrix.yy * FACTOR_16, 0, 0, 0, 1);
                    KIS_SAFE_ASSERT_RECOVER_NOOP(bitmapTf.m11() == bitmapTf.m22());
                    bitmapScale = bitmapTf.m11();
                    QPointF anchor(-currentGlyph.ftface->glyph->bitmap_left, currentGlyph.ftface->glyph->bitmap_top);
                    bitmapTf = QTransform::fromTranslate(-anchor.x(), -anchor.y()) * bitmapTf
                        * QTransform::fromTranslate(anchor.x(), anchor.y());
                }
            } else {
                debugFlake << "Unsupported glyph format" << glyphFormatToStr(currentGlyph.ftface->glyph->format)
                           << "asking freetype to render it for us";
                FT_Render_Mode mode = FT_LOAD_TARGET_MODE(faceLoadFlags);
                if (mode == FT_RENDER_MODE_NORMAL && (faceLoadFlags & FT_LOAD_MONOCHROME)) {
                    mode = FT_RENDER_MODE_MONO;
                }
                if (const FT_Error err = FT_Render_Glyph(currentGlyph.ftface->glyph, mode)) {
                    warnFlake << "Failed to render glyph, freetype error" << err;
                    return false;
                }
            }

            Glyph::Bitmap *bitmapGlyph = std::get_if<Glyph::Bitmap>(&charResult.glyph);
            if (!bitmapGlyph) {
                if (!std::holds_alternative<std::monostate>(charResult.glyph)) {
                    warnFlake << "Glyph contains other type than Bitmap:" << charResult.glyph.index();
                }
                bitmapGlyph = &charResult.glyph.emplace<Glyph::Bitmap>();
            }

            // TODO: Handle glyph clusters better...
            bitmapGlyph->image = convertFromFreeTypeBitmap(currentGlyph.ftface->glyph);

            // Check whether we need to synthesize italic by shearing the glyph:
            if (charResult.fontStyle != QFont::StyleNormal
                && !(currentGlyph.ftface->style_flags & FT_STYLE_FLAG_ITALIC)) {
                // Since we are dealing with a bitmap glyph, we'll just use a nice
                // round floating point number.
                constexpr double SLANT_BITMAP = 0.25;
                QTransform shearTf;
                QPoint shearAt;
                if (isHorizontal) {
                    shearTf.shear(-SLANT_BITMAP, 0);
                    glyphObliqueTf.shear(SLANT_BITMAP, 0);
                    shearAt = QPoint(0, currentGlyph.ftface->glyph->bitmap_top);
                } else {
                    shearTf.shear(0, SLANT_BITMAP);
                    glyphObliqueTf.shear(0, -SLANT_BITMAP);
                    shearAt = QPoint(bitmapGlyph->image.width() / 2, 0);
                }
                // We need to shear around the baseline, hence the translation.
                bitmapTf = (QTransform::fromTranslate(-shearAt.x(), -shearAt.y()) * shearTf
                    * QTransform::fromTranslate(shearAt.x(), shearAt.y())) * bitmapTf;
            }

            if (!bitmapTf.isIdentity()) {
                const QSize srcSize = bitmapGlyph->image.size();
                bitmapGlyph->image = std::move(bitmapGlyph->image).transformed(
                    bitmapTf,
                    this->textRendering == OptimizeSpeed ? Qt::FastTransformation : Qt::SmoothTransformation);

                // This does the same as `QImage::trueMatrix` to get the image
                // offset after transforming.
                const QPoint offset = bitmapTf.mapRect(QRectF({0, 0}, srcSize)).toAlignedRect().topLeft();
                currentGlyph.ftface->glyph->bitmap_left += offset.x();
                currentGlyph.ftface->glyph->bitmap_top -= offset.y();
            }
        }
    }

    {
        charResult.visualIndex = i;
        logicalToVisual.insert(cluster, i);

        charResult.middle = false;
        QPointF advance(currentGlyph.x_advance, currentGlyph.y_advance);
        if (tabSizeInfo.contains(cluster)) {
            KoSvgText::TabSizeInfo tabSize = tabSizeInfo.value(cluster);
            qreal newAdvance = tabSize.value * ftFontUnit;
            if (tabSize.isNumber) {
                QPointF extraSpacing = isHorizontal ? QPointF(tabSize.extraSpacing * ftFontUnit, 0) : QPointF(0, tabSize.extraSpacing * ftFontUnit);
                advance = (spaceAdvance + extraSpacing) * tabSize.value;
            } else {
                advance = isHorizontal ? QPointF(newAdvance, advance.y()) : QPointF(advance.x(), newAdvance);
            }
            charResult.glyph = std::monostate{};
        }
        charResult.advance += ftTF.map(advance);

        Glyph::Bitmap *const bitmapGlyph = std::get_if<Glyph::Bitmap>(&charResult.glyph);

        if (bitmapGlyph) {
            const int width = bitmapGlyph->image.width();
            const int height = bitmapGlyph->image.height();
            const int left = currentGlyph.ftface->glyph->bitmap_left;
            const int top = currentGlyph.ftface->glyph->bitmap_top - height;
            QRect bboxPixel(left, top, width, height);
            if (!isHorizontal) {
                bboxPixel.moveLeft(-(bboxPixel.width() / 2));
            }
            bitmapGlyph->drawRect = ftTF.mapRect(QRectF(bboxPixel.topLeft() * ftFontUnit, bboxPixel.size() * ftFontUnit));
        }

        QRectF bbox;
        if (isHorizontal) {
            bbox = QRectF(0,
                          charResult.fontDescent * bitmapScale,
                          ftTF.inverted().map(charResult.advance).x(),
                          (charResult.fontAscent - charResult.fontDescent) * bitmapScale);
            bbox = glyphObliqueTf.mapRect(bbox);
        } else {
            hb_font_t_up font(hb_ft_font_create_referenced(currentGlyph.ftface));
            bbox = QRectF(charResult.fontDescent * bitmapScale,
                          0,
                          (charResult.fontAscent - charResult.fontDescent) * bitmapScale,
                          ftTF.inverted().map(charResult.advance).y());
            bbox = glyphObliqueTf.mapRect(bbox);
        }
        charResult.boundingBox = ftTF.mapRect(bbox);
        charResult.scaledHalfLeading = ftTF.map(QPointF(charResult.fontHalfLeading, charResult.fontHalfLeading)).x();
        charResult.scaledAscent = isHorizontal? charResult.boundingBox.top(): charResult.boundingBox.right();
        charResult.scaledDescent = isHorizontal? charResult.boundingBox.bottom(): charResult.boundingBox.left();
        if (isHorizontal) {
            charResult.lineHeightBox = charResult.boundingBox.adjusted(0, -charResult.scaledHalfLeading, 0, charResult.scaledHalfLeading);
        } else {
            charResult.lineHeightBox = charResult.boundingBox.adjusted(-charResult.scaledHalfLeading, 0, charResult.scaledHalfLeading, 0);
        }

        if (bitmapGlyph) {
            charResult.boundingBox |= bitmapGlyph->drawRect;
        } else if (const auto *outlineGlyph = std::get_if<Glyph::Outline>(&charResult.glyph)) {
            charResult.boundingBox |= outlineGlyph->path.boundingRect();
        } else if (const auto *colorGlyph = std::get_if<Glyph::ColorLayers>(&charResult.glyph)) {
            Q_FOREACH (const QPainterPath &p, colorGlyph->paths) {
                charResult.boundingBox |= p.boundingRect();
            }
        } else if (!std::holds_alternative<std::monostate>(charResult.glyph)) {
            warnFlake << "Unhandled glyph type" << charResult.glyph.index();
        }
        totalAdvanceFTFontCoordinates += advance;
        charResult.cssPosition = ftTF.map(totalAdvanceFTFontCoordinates) - charResult.advance;
    }
    return true;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static QPainterPath convertFromFreeTypeOutline(FT_GlyphSlotRec *glyphSlot)
{
    QPointF cp = QPointF();
    // convert the outline to a painter path
    // This is taken from qfontengine_ft.cpp.
    QPainterPath glyph;
    glyph.setFillRule(Qt::WindingFill);
    int i = 0;
    for (int j = 0; j < glyphSlot->outline.n_contours; ++j) {
        int last_point = glyphSlot->outline.contours[j];
        // qDebug() << "contour:" << i << "to" << last_point;
        QPointF start = QPointF(glyphSlot->outline.points[i].x, glyphSlot->outline.points[i].y);
        if (!(glyphSlot->outline.tags[i] & 1)) { // start point is not on curve:
            if (!(glyphSlot->outline.tags[last_point] & 1)) { // end point is not on curve:
                // qDebug() << "  start and end point are not on curve";
                start = (QPointF(glyphSlot->outline.points[last_point].x, glyphSlot->outline.points[last_point].y) + start) / 2.0;
            } else {
                // qDebug() << "  end point is on curve, start is not";
                start = QPointF(glyphSlot->outline.points[last_point].x, glyphSlot->outline.points[last_point].y);
            }
            --i; // to use original start point as control point below
        }
        start += cp;
        // qDebug() << "  start at" << start;
        glyph.moveTo(start);
        std::array<QPointF, 4> curve;
        curve[0] = start;
        size_t n = 1;
        while (i < last_point) {
            ++i;
            curve.at(n) = cp + QPointF(glyphSlot->outline.points[i].x, glyphSlot->outline.points[i].y);
            // qDebug() << "    " << i << c[n] << "tag =" <<
            // (int)g->outline.tags[i]
            //                    << ": on curve =" << (bool)(g->outline.tags[i]
            //                    & 1);
            ++n;
            switch (glyphSlot->outline.tags[i] & 3) {
            case 2:
                // cubic bezier element
                if (n < 4)
                    continue;
                curve[3] = (curve[3] + curve[2]) / 2;
                --i;
                break;
            case 0:
                // quadratic bezier element
                if (n < 3)
                    continue;
                curve[3] = (curve[1] + curve[2]) / 2;
                curve[2] = (2 * curve[1] + curve[3]) / 3;
                curve[1] = (2 * curve[1] + curve[0]) / 3;
                --i;
                break;
            case 1:
            case 3:
                if (n == 2) {
                    // qDebug() << "  lineTo" << c[1];
                    glyph.lineTo(curve[1]);
                    curve[0] = curve[1];
                    n = 1;
                    continue;
                } else if (n == 3) {
                    curve[3] = curve[2];
                    curve[2] = (2 * curve[1] + curve[3]) / 3;
                    curve[1] = (2 * curve[1] + curve[0]) / 3;
                }
                break;
            }
            // qDebug() << "  cubicTo" << c[1] << c[2] << c[3];
            glyph.cubicTo(curve[1], curve[2], curve[3]);
            curve[0] = curve[3];
            n = 1;
        }
        if (n == 1) {
            // qDebug() << "  closeSubpath";
            glyph.closeSubpath();
        } else {
            curve[3] = start;
            if (n == 2) {
                curve[2] = (2 * curve[1] + curve[3]) / 3;
                curve[1] = (2 * curve[1] + curve[0]) / 3;
            }
            // qDebug() << "  close cubicTo" << c[1] << c[2] << c[3];
            glyph.cubicTo(curve[1], curve[2], curve[3]);
        }
        ++i;
    }
    return glyph;
}

static QImage convertFromFreeTypeBitmap(FT_GlyphSlotRec *glyphSlot)
{
    KIS_ASSERT(glyphSlot->bitmap.width <= INT32_MAX);
    KIS_ASSERT(glyphSlot->bitmap.rows <= INT32_MAX);
    QImage img;
    const int height = static_cast<int>(glyphSlot->bitmap.rows);
    const QSize size(static_cast<int>(glyphSlot->bitmap.width), height);

    if (glyphSlot->bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
        img = QImage(size, QImage::Format_Mono);
        uchar *src = glyphSlot->bitmap.buffer;
        KIS_ASSERT(glyphSlot->bitmap.pitch >= 0);
        for (int y = 0; y < height; y++) {
            memcpy(img.scanLine(y), src, static_cast<size_t>(glyphSlot->bitmap.pitch));
            src += glyphSlot->bitmap.pitch;
        }
    } else if (glyphSlot->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
        img = QImage(size, QImage::Format_Grayscale8);
        uchar *src = glyphSlot->bitmap.buffer;
        KIS_ASSERT(glyphSlot->bitmap.pitch >= 0);
        for (int y = 0; y < height; y++) {
            memcpy(img.scanLine(y), src, static_cast<size_t>(glyphSlot->bitmap.pitch));
            src += glyphSlot->bitmap.pitch;
        }
    } else if (glyphSlot->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
        img = QImage(size, QImage::Format_ARGB32_Premultiplied);
        const uint8_t *src = glyphSlot->bitmap.buffer;
        for (int y = 0; y < height; y++) {
            auto *argb = reinterpret_cast<QRgb *>(img.scanLine(y));
            for (unsigned int x = 0; x < glyphSlot->bitmap.width; x++) {
                argb[x] = qRgba(src[2], src[1], src[0], src[3]);
                src += 4;
            }
        }
    }

    return img;
}
