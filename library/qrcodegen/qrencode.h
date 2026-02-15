/**
 * qrencode - QR Code encoder
 *
 * Copyright (C) 2006-2017 Kentaro Fukuchi <kentaro@fukuchi.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef QRENCODE_H
#define QRENCODE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Encoding mode.
 */
typedef enum {
  QR_MODE_NUM = 0,    ///< Numeric mode
  QR_MODE_AN,         ///< Alphabet-numeric mode
  QR_MODE_8,          ///< 8-bit data mode
  QR_MODE_KANJI,      ///< Kanji (shift-jis) mode
  QR_MODE_STRUCTURE,  ///< Internal use only
  QR_MODE_ECI,        ///< ECI mode
  QR_MODE_FNC1FIRST,  ///< FNC1, first position
  QR_MODE_FNC1SECOND, ///< FNC1, second position
  QR_MODE_NUL         ///< Terminator (NUL character). Internal use only
} QRencodeMode;

/**
 * Level of error correction.
 */
typedef enum {
  QR_ECLEVEL_L = 0, ///< lowest
  QR_ECLEVEL_M,
  QR_ECLEVEL_Q,
  QR_ECLEVEL_H ///< highest
} QRecLevel;

/**
 * QRcode class.
 * Symbol data is represented as an array contains width*width uchars.
 * Each uchar represents a module (dot). If the less significant bit of
 * the uchar is 1, the corresponding module is black. The other bits are
 * meaningless for usual applications, but here its specification is described.
 *
 * <pre>
 * MSB 76543210 LSB
 *     |||||||`- 1=black/0=white
 *     ||||||`-- data and ecc code area
 *     |||||`--- format information
 *     ||||`---- version information
 *     |||`----- timing pattern
 *     ||`------ alignment pattern
 *     |`------- finder pattern and separator
 *     `-------- non-data modules (format, timing, etc.)
 * </pre>
 */
typedef struct {
  int version;         ///< version of the symbol
  int width;           ///< width of the symbol
  unsigned char *data; ///< symbol data
} QRcode;

/**
 * Create a symbol from the string. The library automatically parses the input
 * string and encodes in a QR Code symbol.
 * @warning This function is THREAD UNSAFE when pthread is disabled.
 * @param string input string. It must be NUL terminated.
 * @param version version of the symbol. If 0, the library chooses the minimum
 *                version for the given input data.
 * @param level error correction level.
 * @param hint tell the library how Japanese Kanji characters should be
 *             encoded. If QR_MODE_KANJI is given, the library assumes that the
 *             given string contains Shift-JIS characters and encodes them in
 *             Kanji-mode. If QR_MODE_8 is given, all of non-alphanumerical
 *             characters will be encoded as is. If you want to embed UTF-8
 *             string, choose this. Other mode will cause EINVAL error.
 * @param casesensitive case-sensitive(1) or not(0).
 * @return an instance of QRcode class. The version of the result QRcode may
 *         be larger than the designated version. On error, NULL is returned,
 *         and errno is set to indicate the error. See Exceptions for the
 *         details.
 * @throw EINVAL invalid input object.
 * @throw ENOMEM unable to allocate memory for input objects.
 */
extern QRcode *QRcode_encodeString(const char *string, int version,
                                   QRecLevel level, QRencodeMode hint,
                                   int casesensitive);

/**
 * Same to QRcode_encodeString(), but encode whole data in 8-bit mode.
 * @warning This function is THREAD UNSAFE when pthread is disabled.
 */
extern QRcode *QRcode_encodeString8bit(const char *string, int version,
                                       QRecLevel level);

/**
 * Encode byte stream (may include '\\0') in 8-bit mode.
 * @warning This function is THREAD UNSAFE when pthread is disabled.
 * @param size size of the input data.
 * @param data input data.
 * @param version version of the symbol. If 0, the library chooses the minimum
 *                version for the given input data.
 * @param level error correction level.
 */
extern QRcode *QRcode_encodeData(int size, const unsigned char *data,
                                 int version, QRecLevel level);

/**
 * Free the instance of QRcode class.
 * @param qrcode an instance of QRcode class.
 */
extern void QRcode_free(QRcode *qrcode);

#ifdef __cplusplus
}
#endif

#endif /* QRENCODE_H */
