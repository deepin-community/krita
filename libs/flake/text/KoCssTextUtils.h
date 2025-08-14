/*
 *  SPDX-FileCopyrightText: 2022 Wolthera van Hövell tot Westerflier <griffinvalley@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KOCSSTEXTUTILS_H
#define KOCSSTEXTUTILS_H

#include <KoSvgText.h>
#include <QDebug>
#include <QLocale>
#include <QString>

#include "kritaflake_export.h"
/**
 * @brief The KoCssTextUtils class
 *
 * This class keeps a number of utility functions related to CSS Text,
 * in particular CSS-Text-3 and CSS-Text-4.
 */
class KRITAFLAKE_EXPORT KoCssTextUtils
{
public:
    /**
     * @brief transformTextToUpperCase
     * convenience function that creates a QLocale and uses it's 'toUpper'
     * function. Note: When building Qt without ICU, this uses platform
     * dependant functions.
     *
     * @param text the text to transform.
     * @param langCode the language code in BCP format, it gets transformed to
     * qLocale's format.
     * @return the transformed string.
     */
    static QString transformTextToUpperCase(const QString &text, const QString &langCode)
    {
        QLocale locale(langCode.split("-").join("_"));
        return locale.toUpper(text);
    };

    /**
     * @brief transformTextToUpperCase
     * convenience function that creates a QLocale and uses it's 'toLower'
     * function. Note: When building Qt without ICU, this uses platform
     * dependant functions.
     *
     * @param text the text to transform.
     * @param langCode the language code in BCP format, it gets transformed to
     * qLocale's format.
     * @return the transformed string.
     */
    static QString transformTextToLowerCase(const QString &text, const QString &langCode)
    {
        QLocale locale(langCode.split("-").join("_"));
        return locale.toLower(text);
    };

    /**
     * @brief transformTextToUpperCase
     * This function splits the text into graphemes, and then uses
     * QLocale::toUpper for each letter following a whitespace character or CSS
     * Wordseparator. It has a small codepath for transforming the Dutch IJ
     * correctly, as this is more readable. Note: When building Qt without ICU,
     * this uses platform dependant functions.
     *
     * @param text the text to transform.
     * @param langCode the language code in BCP format, it gets transformed to
     * qLocale's format.
     * @return the transformed string.
     */
    static QString transformTextCapitalize(const QString &text, QString langCode);

    /**
     * @brief transformTextFullWidth
     * This function will transform 'narrow' or 'halfwidth' characters to their
     * normal counterparts, and will transform ascii characters to their
     * 'fullwidth'/'ideographic' counterparts.
     *
     * @param text the text to transform.
     * @return the transformed text.
     */
    static QString transformTextFullWidth(const QString &text);
    /**
     * @brief transformTextFullSizeKana
     * This function will take 'small' Kana (Japanese phonetic script) and
     * transform it to their 'full-size' equivelants, following the list in the
     * CSS-Text-3 spec.
     *
     * @param text the text to transform.
     * @return the transformed text.
     */
    static QString transformTextFullSizeKana(const QString &text);

    /**
     * @brief collapseSpaces
     * Some versions of CSS-Text 'white-space' or 'text-space-collapse' will
     * collapse or transform white space characters while others don't. This
     * function returns whether that's the case.
     *
     * @param text the text to check against, this text will be transformed if
     * the collapse method requires that.
     * @param collapseMethod the white-space/text-space-collapse method.
     * @return A vector of booleans the size of the input text that marks
     * whether the character should be collapsed.
     */
    static QVector<bool> collapseSpaces(QString *text, KoSvgText::TextSpaceCollapse collapseMethod);

    /**
     * @brief collapseLastSpace
     * Some versions of CSS-Text 'white-space' or 'text-space-collapse' will
     * collapse the last spaces while others don't. This function returns
     * whether that's the case.
     *
     * @param c the character to check.
     * @param collapseMethod the text-space collapse type.
     * @return whether the character should collapse if it's the last space in a
     * line.
     */
    static bool collapseLastSpace(QChar c, KoSvgText::TextSpaceCollapse collapseMethod);

    /**
     * @brief hangLastSpace
     * Some versions of CSS-Text 'white-space' or 'text-space-collapse' will
     * hang the final space depending on the situation.
     * @param c the character in question.
     * @param collapseMethod the collapse method
     * @param wrapMethod the wrap method.
     * @param force whether said hang is a forced hang or not.
     * @param nextCharIsHardBreak whether the next char is a line break.
     * @return
     */
    static bool hangLastSpace(const QChar c,
                              KoSvgText::TextSpaceCollapse collapseMethod,
                              KoSvgText::TextWrap wrapMethod,
                              bool &force, bool nextCharIsHardBreak);

    /**
     * @brief characterCanHang
     * The function returns whether the character qualifies for
     * 'hanging-punctuation', using the given hang-type.
     *
     * @param c the character to check.
     * @param hangType how to hang.
     * @return whether the character can hang.
     */
    static bool characterCanHang(QChar c, KoSvgText::HangingPunctuations hangType);

    /**
     * @brief IsCssWordSeparator
     * CSS has a number of characters it considers word-separators, which are
     * used in justification and for word-spacing.
     *
     * @param grapheme a grapheme to check. Using graphemes here, because some
     * of the word-separators are not in the unicode basic plane.
     * @return true if it is a word-separator
     */
    static bool IsCssWordSeparator(QString grapheme);

    /**
     * @brief textToUnicodeGraphemes
     * In letters like Å, the amount of unicode codpoints can be 1, but it can
     * also be 2, one for 'A', and one for 'Combining Mark Ring Above". In some
     * letters used by Vietnamese, such as ỗ there can be even 3. Such codepoint
     * sequences are considered 'grapheme-clusters'. For editing text, matching
     * fonts or capitalizing the first letter, it's wisest to do so on the
     * grapheme clusters instead of the individual codepoints.
     *
     * @param text the text to break.
     * @param langCode the language code of the text, BCP style.
     * @return a QStringList of the graphemes as seperate strings.
     */
    static QStringList textToUnicodeGraphemeClusters(QString text, QString langCode);

    /**
     * @brief justificationOpportunities
     * mark justification opportunities in the text. Opportunities are between
     * characters, so this returns a pair of before and after.
     * As of currently, this only implements the bare minimum for CSS-Text-3
     * auto justification.
     * @param text the text to check against.
     * @param langCode language, used for the grapheme breaking.
     * @return a list of booleans for whether the current codePoint represents a justificaton opportunity.
     */
    static QVector<QPair<bool, bool>> justificationOpportunities(QString text, QString langCode);
};

#endif // KOCSSTEXTUTILS_H
