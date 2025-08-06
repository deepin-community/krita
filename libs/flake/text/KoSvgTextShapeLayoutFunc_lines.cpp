/*
 *  SPDX-FileCopyrightText: 2017 Dmitry Kazakov <dimula73@gmail.com>
 *  SPDX-FileCopyrightText: 2022 Wolthera van Hövell tot Westerflier <griffinvalley@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KoSvgTextShapeLayoutFunc.h"

#include "KoSvgTextProperties.h"

#include <FlakeDebug.h>

namespace KoSvgTextShapeLayoutFunc
{

/**
 * @brief calculateLineHeight
 * calculate the total ascent and descent (including baseline-offset) of a charResult
 * and optionally only return it if it is larger than the provided ascent and descent
 * variable.
 * This is necessary for proper line-height calculation.
 * @param cr - char result with the data.
 * @param ascent - output ascent variable.
 * @param descent - output descent variable.
 * @param isHorizontal - whether it is horizontal.
 * @param compare - whether to only return the value if it is larger than the relative ascent or descent.
 */
void calculateLineHeight(CharacterResult cr, double &ascent, double &descent, bool isHorizontal, bool compare)
{
    double offset = isHorizontal? cr.baselineOffset.y(): cr.baselineOffset.x();
    double offsetAsc = 0.0;
    double offsetDsc = 0.0;
    if (cr.scaledAscent <= 0) {
        offsetAsc = cr.scaledAscent - cr.scaledHalfLeading;
        offsetDsc = cr.scaledDescent + cr.scaledHalfLeading;
    } else {
        offsetAsc = cr.scaledAscent + cr.scaledHalfLeading;
        offsetDsc = cr.scaledDescent - cr.scaledHalfLeading;
    }
    offsetAsc += offset;
    offsetDsc += offset;

    if (!compare) {
        ascent = offsetAsc;
        descent = offsetDsc;
    } else {
        if (cr.scaledAscent <= 0) {
            ascent = qMin(offsetAsc, ascent);
            descent = qMax(offsetDsc, descent);
        } else {
            ascent = qMax(offsetAsc, ascent);
            descent = qMin(offsetDsc, descent);
        }
    }
}

/**
 * @brief addWordToLine
 * Small function used in break lines to quickly add a 'word' to the current
 * line. Returns the last added index.
 */
void addWordToLine(QVector<CharacterResult> &result,
                   QPointF &currentPos,
                   QVector<int> &wordIndices,
                   LineBox &currentLine,
                   bool ltr,
                   bool isHorizontal)
{
    QPointF lineAdvance = currentPos;

    LineChunk currentChunk  = currentLine.chunk();

    Q_FOREACH (const int j, wordIndices) {
        CharacterResult cr = result.at(j);
        if (currentChunk.boundingBox.isEmpty() && j == wordIndices.first()) {
            if (result.at(j).lineStart == LineEdgeBehaviour::Collapse) {
                result[j].addressable = false;
                result[j].hidden = true;
                continue;
            }
            cr.anchored_chunk = true;
            if (result.at(j).lineStart == LineEdgeBehaviour::ForceHang && currentLine.firstLine) {
                currentPos -= cr.advance;
                cr.isHanging = true;
            }

            // Ensure that the first non-collapsed result will always set the line-top and bottom.
            calculateLineHeight(cr, currentLine.actualLineTop, currentLine.actualLineBottom, isHorizontal, false);
        } else {
            calculateLineHeight(cr, currentLine.actualLineTop, currentLine.actualLineBottom, isHorizontal, true);
        }
        cr.cssPosition = currentPos;
        currentPos += cr.advance;
        lineAdvance = currentPos;

        result[j] = cr;
        currentChunk.boundingBox |= cr.boundingBox.translated(cr.cssPosition + cr.baselineOffset);
    }
    currentPos = lineAdvance;
    currentChunk.chunkIndices += wordIndices;
    currentLine.setCurrentChunk(currentChunk);
    wordIndices.clear();
}

/**
 * This offsets the last line by it's ascent, and then returns the last line's
 * descent.
 */
static QPointF lineHeightOffset(KoSvgText::WritingMode writingMode,
                                QVector<CharacterResult> &result,
                                LineBox &currentLine,
                                bool firstLine)
{
    QPointF lineTop;
    QPointF lineBottom;
    QPointF correctionOffset; ///< This is for determining the difference between
                              ///< a predicted line-height (for text-in-shape) and the actual line-height.

    if (currentLine.chunks.isEmpty()) {
        return QPointF();
    } else if (currentLine.chunks.size() == 1 && currentLine.actualLineTop == 0 &&
               currentLine.actualLineBottom == 0){
        /**
         * When the line is empty, but caused by a hardbreak, we will need to use that hardbreak
         * to space the line. This can only be done at this point as it would otherwise need to use
         * visible characters.
         */
        QVector<int> chunkIndices = currentLine.chunks[0].chunkIndices;
        if (chunkIndices.size() > 0) {
            CharacterResult cr = result[chunkIndices.first()];
            calculateLineHeight(cr,
                                currentLine.actualLineTop,
                                currentLine.actualLineBottom,
                                writingMode == KoSvgText::HorizontalTB,
                                false);
        }
    }

    qreal expectedLineTop = currentLine.actualLineTop > 0? qMax(currentLine.expectedLineTop, currentLine.actualLineTop):
                                                       qMin(currentLine.expectedLineTop, currentLine.actualLineTop);

    if (writingMode == KoSvgText::HorizontalTB) {
        currentLine.baselineTop = QPointF(0, currentLine.actualLineTop);
        currentLine.baselineBottom = QPointF(0, currentLine.actualLineBottom);
        correctionOffset = QPointF(0, expectedLineTop) - currentLine.baselineTop;
        lineTop = -currentLine.baselineTop;
        lineBottom = currentLine.baselineBottom;
    } else if (writingMode == KoSvgText::VerticalLR) {
        currentLine.baselineTop = QPointF(currentLine.actualLineTop, 0);
        currentLine.baselineBottom = QPointF(currentLine.actualLineBottom, 0);
        correctionOffset = QPointF(expectedLineTop, 0) - currentLine.baselineTop;
        // Note: while Vertical LR goes left-to-right in its lines, its lines themselves are
        // oriented with the top pointed in the positive x direction.
        lineBottom = currentLine.baselineTop;
        lineTop = -currentLine.baselineBottom;
    } else {
        currentLine.baselineTop = QPointF(currentLine.actualLineTop, 0);
        currentLine.baselineBottom = QPointF(currentLine.actualLineBottom, 0);
        correctionOffset = QPointF(-expectedLineTop, 0) + currentLine.baselineTop;
        lineTop = -currentLine.baselineTop;
        lineBottom = currentLine.baselineBottom;
    }
    bool returnDescent = firstLine;
    QPointF offset = lineTop + lineBottom;

    if (!returnDescent) {
        for (LineChunk &chunk : currentLine.chunks) {
            Q_FOREACH (int j, chunk.chunkIndices) {
                result[j].cssPosition += lineTop;
                result[j].cssPosition += result[j].baselineOffset;
                result[j].finalPosition = result.at(j).cssPosition;
            }
            chunk.length.translate(lineTop);
            chunk.boundingBox.translate(lineTop);
        }
    } else {
        offset = lineBottom - correctionOffset;
        for (LineChunk &chunk : currentLine.chunks) {
            Q_FOREACH (int j, chunk.chunkIndices) {
                result[j].cssPosition -= correctionOffset;
                result[j].cssPosition +=  result[j].baselineOffset;
                result[j].finalPosition = result.at(j).cssPosition;
            }
            chunk.length.translate(-correctionOffset);
            chunk.boundingBox.translate(-correctionOffset);
        }
    }
    return offset;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void
handleCollapseAndHang(QVector<CharacterResult> &result, LineChunk &chunk, bool ltr, bool isHorizontal)
{
    QVector<int> lineIndices = chunk.chunkIndices;
    QPointF endPos = chunk.length.p2();

    if (!lineIndices.isEmpty()) {
        QVectorIterator<int> it(lineIndices);
        it.toBack();
        while (it.hasPrevious()) {
            int lastIndex = it.previous();
            if (result.at(lastIndex).lineEnd == LineEdgeBehaviour::Collapse) {
                result[lastIndex].addressable = false;
                result[lastIndex].hidden = true;
            } else if (result.at(lastIndex).lineEnd == LineEdgeBehaviour::ConditionallyHang) {
                if (ltr) {
                    QPointF hangPos = result[lastIndex].cssPosition + result[lastIndex].advance;
                    if (isHorizontal) {
                        if (hangPos.x() > endPos.x()) {
                            result[lastIndex].isHanging = true;
                            chunk.conditionalHangEnd = hangPos - endPos;
                        }
                    } else {
                        if (hangPos.y() > endPos.y()) {
                            result[lastIndex].isHanging = true;
                            chunk.conditionalHangEnd = hangPos - endPos;
                        }
                    }
                } else {
                    QPointF hangPos = result[lastIndex].cssPosition;
                    if (hangPos.x() < endPos.x()) {
                        result[lastIndex].isHanging = true;
                        chunk.conditionalHangEnd = hangPos - endPos;
                    }
                }

            } else if (result.at(lastIndex).lineEnd == LineEdgeBehaviour::ForceHang) {
                result[lastIndex].isHanging = true;
                chunk.conditionalHangEnd = result[lastIndex].advance;
            }
            if (result.at(lastIndex).lineEnd != LineEdgeBehaviour::Collapse) {
                break;
            }
        }
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void applyInlineSizeAnchoring(QVector<CharacterResult> &result,
                                     LineChunk chunk,
                                     KoSvgText::TextAnchor anchor,
                                     QPointF anchorPoint,
                                     bool ltr,
                                     bool isHorizontal,
                                     QPointF textIndent)
{
    QVector<int> lineIndices = chunk.chunkIndices;
    QPointF startPos = anchorPoint;
    qreal shift = isHorizontal ? startPos.x() : startPos.y();

    qreal a = 0;
    qreal b = 0;

    bool first = true;
    Q_FOREACH (int i, lineIndices) {
        if (!result.at(i).addressable || (result.at(i).isHanging && result.at(i).anchored_chunk)) {
            continue;
        }

        QPointF p = result.at(i).finalPosition;
        QPointF d = result.at(i).advance;
        if (result.at(i).isHanging) {
            d -= chunk.conditionalHangEnd;
            if (!ltr) {
                p += chunk.conditionalHangEnd;
            }
        }
        const qreal pos = isHorizontal ? p.x() : p.y();
        const qreal advance = isHorizontal ? d.x() : d.y();

        if (first) {
            a = qMin(pos, pos + advance);
            b = qMax(pos, pos + advance);
            first = false;
        } else {
            a = qMin(a, qMin(pos, pos + advance));
            b = qMax(b, qMax(pos, pos + advance));
        }
    }


    if (anchor == KoSvgText::AnchorStart) {
        const qreal indent = isHorizontal ? textIndent.x() : textIndent.y();
        if (ltr) {
            a -= indent;
        } else {
            b += indent;
        }
    }

    if ((anchor == KoSvgText::AnchorStart && ltr) || (anchor == KoSvgText::AnchorEnd && !ltr)) {
        shift -= a;

    } else if ((anchor == KoSvgText::AnchorEnd && ltr) || (anchor == KoSvgText::AnchorStart && !ltr)) {
        shift -= b;

    } else {
        shift -= ((a + b) * 0.5);
    }

    QPointF shiftP = isHorizontal ? QPointF(shift, 0) : QPointF(0, shift);
    Q_FOREACH (int j, lineIndices) {
        result[j].cssPosition += shiftP;
        result[j].finalPosition = result.at(j).cssPosition;
    }
}

/// Finalizing the line consists of several steps, like hang/collapse, anchoring
/// into place and offsetting correctly. This can happen several times during a
/// linebreak, hence this convenience function to handle this.
void finalizeLine(QVector<CharacterResult> &result,
                  QPointF &currentPos,
                  LineBox &currentLine,
                  QPointF &lineOffset,
                  KoSvgText::TextAnchor anchor,
                  KoSvgText::WritingMode writingMode,
                  bool ltr,
                  bool inlineSize,
                  bool textInShape)
{
    bool isHorizontal = writingMode == KoSvgText::HorizontalTB;

    bool firstLine = textInShape? true: currentLine.firstLine;

    Q_FOREACH (LineChunk currentChunk, currentLine.chunks) {
        QMap<int, int> visualToLogical;
        Q_FOREACH (int j, currentChunk.chunkIndices) {
            visualToLogical.insert(result.at(j).visualIndex, j);
        }
        currentPos = lineOffset;

        handleCollapseAndHang(result, currentChunk, ltr, isHorizontal);

        QPointF justifyOffset;
        QVector<int> before;
        QVector<int> after;

        if (currentLine.justifyLine) {
            double hangingGlyphLength = isHorizontal? currentChunk.conditionalHangEnd.x(): currentChunk.conditionalHangEnd.y();
            QPointF advanceLength; ///< Because we may have collapsed the last glyph, we'll need to recalculate the total advance;
            bool first = true;
            Q_FOREACH (int j, visualToLogical.values()) {
                if (!result.at(j).addressable) {
                    continue;
                }
                advanceLength += result.at(j).advance;
                if (result.at(j).isHanging) {
                    if (result.at(j).anchored_chunk) {
                        hangingGlyphLength += isHorizontal? result.at(j).advance.x(): result.at(j).advance.y();
                    }
                    continue;
                }
                bool last = visualToLogical.values().last() == j;
                if (!last) {
                    last = result.at(j+1).isHanging;
                }

                if (result.at(j).justifyBefore && !first) {
                    before.append(j);
                }
                if (result.at(j).justifyAfter && !last) {
                    after.append(j);
                }
                first = false;
            }

            int justificationCount = before.size()+after.size();
            if (justificationCount > 0) {
                if (isHorizontal) {
                    double val = currentChunk.length.length() + hangingGlyphLength - advanceLength.x();
                    val = val / justificationCount;
                    justifyOffset = QPointF(val, 0);
                } else {
                    double val = currentChunk.length.length() + hangingGlyphLength - advanceLength.y();
                    val = val / justificationCount;
                    justifyOffset = QPointF(0, val);
                }
            }
        }

        Q_FOREACH (const int j, visualToLogical.values()) {
            if (!result.at(j).addressable) {
                continue;
            }
            if ((result.at(j).isHanging && result.at(j).anchored_chunk)) {
                if (ltr) {
                    result[j].cssPosition = currentPos - result.at(j).advance;
                    result[j].finalPosition = result[j].cssPosition;
                } else {
                    result[j].cssPosition = currentPos;
                    result[j].finalPosition = result[j].cssPosition;
                }
            } else {
                if (before.contains(j)) {
                    currentPos += justifyOffset;
                }
                result[j].cssPosition = currentPos;
                result[j].finalPosition = currentPos;
                currentPos = currentPos + result.at(j).advance;
                if (after.contains(j)) {
                    currentPos += justifyOffset;
                }
            }
        }

        if (inlineSize) {
            QPointF anchorPoint = currentChunk.length.p1();
            if (textInShape) {
                if (anchor == KoSvgText::AnchorMiddle) {
                    anchorPoint = currentChunk.length.center();
                } else if (anchor == KoSvgText::AnchorEnd) {
                    anchorPoint = currentChunk.length.p2();
                }
            }
            applyInlineSizeAnchoring(result, currentChunk, anchor, anchorPoint, ltr, isHorizontal, currentLine.textIndent);
        }
    }
    lineOffset += lineHeightOffset(writingMode, result, currentLine, firstLine);
    currentPos = lineOffset;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
QVector<LineBox> breakLines(const KoSvgTextProperties &properties,
                            const QMap<int, int> &logicalToVisual,
                            QVector<CharacterResult> &result,
                            QPointF startPos)
{
    KoSvgText::WritingMode writingMode = KoSvgText::WritingMode(properties.propertyOrDefault(KoSvgTextProperties::WritingModeId).toInt());
    KoSvgText::Direction direction = KoSvgText::Direction(properties.propertyOrDefault(KoSvgTextProperties::DirectionId).toInt());
    KoSvgText::AutoValue inlineSize = properties.propertyOrDefault(KoSvgTextProperties::InlineSizeId).value<KoSvgText::AutoValue>();
    KoSvgText::TextAnchor anchor = KoSvgText::TextAnchor(properties.propertyOrDefault(KoSvgTextProperties::TextAnchorId).toInt());

    bool ltr = direction == KoSvgText::DirectionLeftToRight;
    bool isHorizontal = writingMode == KoSvgText::HorizontalTB;

    QVector<LineBox> lineBoxes;

    QPointF endPos; ///< Used for hanging glyphs at the end of a line.

    KoSvgText::TextIndentInfo textIndentInfo = properties.propertyOrDefault(KoSvgTextProperties::TextIndentId).value<KoSvgText::TextIndentInfo>();
    QPointF textIndent;
    if (!inlineSize.isAuto) {
        qreal textIdentValue = textIndentInfo.value;
        if (textIndentInfo.isPercentage) {
            textIndent *= inlineSize.customValue;
        }
        if (isHorizontal) {
            textIndent = QPointF(textIdentValue, 0);
            endPos = ltr ? QPointF(startPos.x() + inlineSize.customValue, 0) : QPointF(startPos.x() - inlineSize.customValue, 0);
        } else {
            textIndent = QPointF(0, textIdentValue);
            endPos = ltr ? QPointF(0, startPos.y() + inlineSize.customValue) : QPointF(0, startPos.y() - inlineSize.customValue);
        }
    }
    LineBox currentLine(startPos, endPos);
    currentLine.firstLine = true;

    QVector<int> wordIndices; ///< 'word' in this case meaning characters
                              ///< inbetween softbreaks.
    QPointF wordAdvance; ///< Approximated advance of the current wordindices.

    QPointF currentPos = startPos; ///< Current position with advances of each character.
    if (!textIndentInfo.hanging && !inlineSize.isAuto) {
        currentLine.textIndent = textIndent;
        currentPos += currentLine.textIndent;
    }
    QPointF lineOffset = startPos; ///< Current line offset.

    QVector<int> lineIndices; ///< Indices of characters in line.

    QListIterator<int> it(logicalToVisual.keys());
    while (it.hasNext()) {
        int index = it.next();
        CharacterResult charResult = result.at(index);
        if (!charResult.addressable) {
            continue;
        }
        bool softBreak = false; ///< Whether to do a softbreak;
        bool doNotCountAdvance =
            ((charResult.lineEnd != LineEdgeBehaviour::NoChange)
             && !(currentLine.isEmpty() && wordIndices.isEmpty()));
        if (!doNotCountAdvance) {
            if (wordIndices.isEmpty()) {
                wordAdvance = charResult.advance;
            } else {
                wordAdvance += charResult.advance;
            }
        }
        wordIndices.append(index);
        currentLine.lastLine = !it.hasNext();

        if (charResult.breakType != BreakType::NoBreak || currentLine.lastLine) {
            qreal lineLength = isHorizontal ? (currentPos - startPos + wordAdvance).x()
                                            : (currentPos - startPos + wordAdvance).y();
            if (!inlineSize.isAuto) {
                // Sometimes glyphs are a fraction larger than you'd expect, but
                // not enough to really break the line, so the following is a
                // bit more stable than a simple compare.
                if (abs(lineLength) - inlineSize.customValue > 0.01) {
                    softBreak = true;
                } else {
                    addWordToLine(result, currentPos, wordIndices, currentLine, ltr, isHorizontal);
                }
            } else {
                addWordToLine(result, currentPos, wordIndices, currentLine, ltr, isHorizontal);
            }
        }

        if (softBreak) {
            bool firstLine = currentLine.firstLine;
            if (!currentLine.isEmpty()) {
                finalizeLine(result,
                             currentPos,
                             currentLine,
                             lineOffset,
                             anchor,
                             writingMode,
                             ltr,
                             !inlineSize.isAuto,
                             false);
                lineBoxes.append(currentLine);
                currentLine.clearAndAdjust(isHorizontal, lineOffset, textIndentInfo.hanging? textIndent: QPointF());
                if (!inlineSize.isAuto) {
                    currentPos += currentLine.textIndent;
                }
            }

            if (charResult.overflowWrap) {
                qreal wordLength = isHorizontal ? wordAdvance.x() : wordAdvance.y();
                if (!inlineSize.isAuto && wordLength > inlineSize.customValue) {
                    // Word is too large, so we try to add it in
                    // max-width-friendly-chunks.
                    wordAdvance = QPointF();
                    wordLength = 0;
                    QVector<int> partialWord;
                    currentLine.firstLine = firstLine;
                    Q_FOREACH (const int i, wordIndices) {
                        wordAdvance += result.at(i).advance;
                        wordLength = isHorizontal ? wordAdvance.x() : wordAdvance.y();
                        if (wordLength <= inlineSize.customValue) {
                            partialWord.append(i);
                        } else {
                            addWordToLine(result, currentPos, partialWord, currentLine, ltr, isHorizontal);

                            finalizeLine(result,
                                         currentPos,
                                         currentLine,
                                         lineOffset,
                                         anchor,
                                         writingMode,
                                         ltr,
                                         !inlineSize.isAuto,
                                         false);
                            lineBoxes.append(currentLine);
                            currentLine.clearAndAdjust(isHorizontal, lineOffset, textIndentInfo.hanging? textIndent: QPointF());
                            if (!inlineSize.isAuto) {
                                currentPos += currentLine.textIndent;
                            }
                            wordAdvance = result.at(i).advance;
                            partialWord.append(i);
                        }
                    }
                    wordIndices = partialWord;
                }
            }
            addWordToLine(result, currentPos, wordIndices, currentLine, ltr, isHorizontal);
        }

        if (charResult.breakType == BreakType::HardBreak) {
            finalizeLine(result,
                         currentPos,
                         currentLine,
                         lineOffset,
                         anchor,
                         writingMode,
                         ltr,
                         !inlineSize.isAuto,
                         false);
            lineBoxes.append(currentLine);
            bool indentLine = textIndentInfo.hanging? false: textIndentInfo.eachLine;
            currentLine.clearAndAdjust(isHorizontal, lineOffset, indentLine? textIndent: QPointF());
            if (!inlineSize.isAuto) {
                currentPos += currentLine.textIndent;
            }
        }

        if (currentLine.lastLine) {
            if (!wordIndices.isEmpty()) {
                addWordToLine(result, currentPos, wordIndices, currentLine, ltr, isHorizontal);
            }
            finalizeLine(result,
                         currentPos,
                         currentLine,
                         lineOffset,
                         anchor,
                         writingMode,
                         ltr,
                         !inlineSize.isAuto,
                         false);
            lineBoxes.append(currentLine);
        }
    }
    debugFlake << "Linebreaking finished";
    return lineBoxes;
}

} // namespace KoSvgTextShapeLayoutFunc
