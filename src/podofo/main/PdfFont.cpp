/**
 * SPDX-FileCopyrightText: (C) 2005 Dominik Seichter <domseichter@web.de>
 * SPDX-FileCopyrightText: (C) 2020 Francesco Pretto <ceztko@gmail.com>
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <podofo/private/PdfDeclarationsPrivate.h>
#include "PdfFont.h"

#include <utf8cpp/utf8.h>

#include <podofo/private/PdfEncodingPrivate.h>
#include <podofo/private/PdfStandard14FontData.h>
#include <podofo/private/outstringstream.h>

#include "PdfArray.h"
#include "PdfEncoding.h"
#include "PdfEncodingFactory.h"
#include <podofo/auxiliary/InputStream.h>
#include "PdfObjectStream.h"
#include "PdfCharCodeMap.h"
#include "PdfFontMetrics.h"
#include "PdfPage.h"
#include "PdfFontMetricsStandard14.h"
#include "PdfFontManager.h"
#include "PdfFontMetricsFreetype.h"
#include "PdfDocument.h"
#include "PdfStringStream.h"

using namespace std;
using namespace cmn;
using namespace PoDoFo;

static double getGlyphLength(double glyphLength, const PdfTextState& state, bool ignoreCharSpacing);
static string_view toString(PdfFontStretch stretch);

PdfFont::PdfFont(PdfDocument& doc, const PdfFontMetricsConstPtr& metrics,
        const PdfEncoding& encoding) :
    PdfDictionaryElement(doc, "Font"_n),
    m_WordSpacingLengthRaw(-1),
    m_SpaceCharLengthRaw(-1),
    m_Metrics(metrics)
{
    if (metrics == nullptr)
        PODOFO_RAISE_ERROR_INFO(PdfErrorCode::InvalidHandle, "Metrics must me not null");

    this->initBase(encoding);
}

PdfFont::PdfFont(PdfObject& obj, const PdfFontMetricsConstPtr& metrics,
        const PdfEncoding& encoding) :
    PdfDictionaryElement(obj),
    m_WordSpacingLengthRaw(-1),
    m_SpaceCharLengthRaw(-1),
    m_Metrics(metrics)
{
    if (metrics == nullptr)
        PODOFO_RAISE_ERROR_INFO(PdfErrorCode::InvalidHandle, "Metrics must me not null");

    this->initBase(encoding);
}

PdfFont::~PdfFont() { }

bool PdfFont::TryCreateSubstituteFont(PdfFont*& substFont) const
{
    return TryCreateSubstituteFont(PdfFontCreateFlags::None, substFont);
}

bool PdfFont::TryCreateSubstituteFont(PdfFontCreateFlags initFlags, PdfFont*& substFont) const
{
    auto encoding = GetEncoding();
    auto& metrics = GetMetrics();
    PdfFontMetricsConstPtr newMetrics;
    if (metrics.HasFontFileData())
    {
        newMetrics = PdfFontMetricsFreetype::CreateSubstituteMetrics(metrics, GetDocument().GetMetadata().GetPdfALevel());
    }
    else
    {
        // Early intercept Standard14 fonts
        PdfStandard14FontType std14Font;
        if (m_Metrics->IsStandard14FontMetrics(std14Font) ||
            PdfFont::IsStandard14Font(metrics.GetFontName(), false, std14Font))
        {
            newMetrics = PdfFontMetricsStandard14::GetInstance(std14Font);
        }
        else
        {
            PdfFontSearchParams params;
            params.Style = metrics.GetStyle();
            params.FontFamilyPattern = metrics.GeFontFamilyNameSafe();
            newMetrics = PdfFontManager::SearchFontMetrics(metrics.GetPostScriptNameRough(), params);
            if (newMetrics == nullptr)
            {
                substFont = nullptr;
                return false;
            }
        }
    }

    if (!encoding.HasValidToUnicodeMap())
    {
        shared_ptr<PdfCMapEncoding> toUnicode = newMetrics->CreateToUnicodeMap(encoding.GetLimits());
        encoding = PdfEncoding(encoding.GetEncodingMapPtr(), toUnicode);
    }

    PdfFontCreateParams params;
    params.Encoding = encoding;
    params.Flags = initFlags;
    auto newFont = PdfFont::Create(GetDocument(), newMetrics, params);
    if (newFont == nullptr)
    {
        substFont = nullptr;
        return false;
    }

    substFont = GetDocument().GetFonts().AddImported(std::move(newFont));
    return true;
}

void PdfFont::initBase(const PdfEncoding& encoding)
{
    m_IsEmbedded = false;
    m_EmbeddingEnabled = false;
    m_SubsettingEnabled = false;
    m_cidToGidMap = m_Metrics->GetCIDToGIDMap();

    if (encoding.IsNull())
    {
        m_DynamicCIDMap = std::make_shared<PdfCharCodeMap>();
        m_DynamicToUnicodeMap = std::make_shared<PdfCharCodeMap>();
        m_Encoding = PdfEncoding::CreateDynamicEncoding(m_DynamicCIDMap, m_DynamicToUnicodeMap, *this);
    }
    else
    {
        m_Encoding = PdfEncoding::CreateSchim(encoding, *this);
    }

    // By default ensure the font has the /BaseFont name or /FontName
    // or, the name inferred from a font file
    m_Name = m_Metrics->GetFontName();
}

void PdfFont::WriteStringToStream(OutputStream& stream, const string_view& str) const
{
    // Optimize serialization for simple encodings
    auto encoded = m_Encoding->ConvertToEncoded(str);
    if (m_Encoding->IsSimpleEncoding())
        utls::SerializeEncodedString(stream, encoded, false);
    else
        utls::SerializeEncodedString(stream, encoded, true);
}

void PdfFont::InitImported(bool wantEmbed, bool wantSubset)
{
    PODOFO_ASSERT(!IsObjectLoaded());

    // No embedding implies no subsetting
    m_EmbeddingEnabled = wantEmbed;
    m_SubsettingEnabled = wantEmbed && wantSubset && SupportsSubsetting();
    if (m_SubsettingEnabled)
    {
        m_SubstGIDMap.reset(new GIDMap());

        // If it exist a glyph for the space character,
        // add it for subsetting. NOTE: Search the GID
        // in the font program
        unsigned gid;
        char32_t spaceCp = U' ';
        if (TryGetGID(spaceCp, PdfGlyphAccess::FontProgram, gid))
        {
            unicodeview codepoints(&spaceCp, 1);
            PdfCID cid;
            (void)tryAddSubsetGID(gid, codepoints, cid);
        }
    }

    unsigned char subsetPrefixLength = m_Metrics->GetSubsetPrefixLength();
    if (subsetPrefixLength == 0)
    {
        if (m_SubsettingEnabled)
        {
            m_SubsetPrefix = GetDocument().GetFonts().GenerateSubsetPrefix();
            m_Name = m_SubsetPrefix.append(m_Metrics->GetPostScriptNameRough());
        }
        else
        {
            m_Name = (string)m_Metrics->GetPostScriptNameRough();
        }
    }
    else
    {
        m_Name = m_Metrics->GetFontName();
        m_SubsetPrefix = m_Name.substr(0, subsetPrefixLength);
    }

    initImported();
}

void PdfFont::EmbedFont()
{
    if (m_IsEmbedded || !m_EmbeddingEnabled)
        return;

    if (m_SubsettingEnabled)
        embedFontSubset();
    else
        embedFont();

    m_IsEmbedded = true;
}

void PdfFont::embedFont()
{
    PODOFO_RAISE_ERROR_INFO(PdfErrorCode::NotImplemented, "Embedding not implemented for this font type");
}

void PdfFont::embedFontSubset()
{
    PODOFO_RAISE_ERROR_INFO(PdfErrorCode::NotImplemented, "Subsetting not implemented for this font type");
}

unsigned PdfFont::GetGID(char32_t codePoint, PdfGlyphAccess access) const
{
    unsigned gid;
    if (!TryGetGID(codePoint, access, gid))
        PODOFO_RAISE_ERROR_INFO(PdfErrorCode::InvalidFontData, "Can't find a gid");

    return gid;
}

bool PdfFont::TryGetGID(char32_t codePoint, PdfGlyphAccess access, unsigned& gid) const
{
    if (IsObjectLoaded() || !m_Metrics->HasUnicodeMapping())
    {
        PdfCharCode codeUnit;
        unsigned cid;
        if (!m_Encoding->GetToUnicodeMapSafe().TryGetCharCode(codePoint, codeUnit)
            || !m_Encoding->TryGetCIDId(codeUnit, cid))
        {
            gid = 0;
            return false;
        }

        return TryMapCIDToGID(cid, access, gid);
    }
    else
    {
        return m_Metrics->TryGetGID(codePoint, gid);
    }
}

double PdfFont::GetStringLength(const string_view& str, const PdfTextState& state) const
{
    // Ignore failures
    double length;
    (void)TryGetStringLength(str, state, length);
    return length;
}

bool PdfFont::TryGetStringLength(const string_view& str, const PdfTextState& state, double& length) const
{
    vector<unsigned> gids;
    bool success = tryConvertToGIDs(str, PdfGlyphAccess::Width, gids);
    length = 0;
    for (unsigned i = 0; i < gids.size(); i++)
        length += getGlyphLength(m_Metrics->GetGlyphWidth(gids[i]), state, false);

    return success;
}

double PdfFont::GetEncodedStringLength(const PdfString& encodedStr, const PdfTextState& state) const
{
    // Ignore failures
    double length;
    (void)TryGetEncodedStringLength(encodedStr, state, length);
    return length;
}

bool PdfFont::TryGetEncodedStringLength(const PdfString& encodedStr, const PdfTextState& state, double& length) const
{
    vector<PdfCID> cids;
    bool success = true;
    if (!m_Encoding->TryConvertToCIDs(encodedStr, cids))
        success = false;

    length = getStringLength(cids, state);
    return success;
}

bool PdfFont::TryScanEncodedString(const PdfString& encodedStr, const PdfTextState& state, string& utf8str, vector<double>& lengths, vector<unsigned>& positions) const
{
    utf8str.clear();
    lengths.clear();
    positions.clear();

    if (encodedStr.IsEmpty())
        return true;

    auto context = m_Encoding->StartStringScan(encodedStr);
    CodePointSpan codepoints;
    PdfCID cid;
    bool success = true;
    unsigned prevOffset = 0;
    double length;
    while (!context.IsEndOfString())
    {
        if (!context.TryScan(cid, utf8str, codepoints))
            success = false;

        length = getGlyphLength(GetCIDLengthRaw(cid.Id), state, false);
        lengths.push_back(length);
        positions.push_back(prevOffset);
        prevOffset = (unsigned)utf8str.length();
    }

    return success;
}

double PdfFont::GetWordSpacingLength(const PdfTextState& state) const
{
    const_cast<PdfFont&>(*this).initSpaceDescriptors();
    return getGlyphLength(m_WordSpacingLengthRaw, state, false);
}

double PdfFont::GetSpaceCharLength(const PdfTextState& state) const
{
    const_cast<PdfFont&>(*this).initSpaceDescriptors();
    return getGlyphLength(m_SpaceCharLengthRaw, state, false);
}

double PdfFont::GetCharLength(char32_t codePoint, const PdfTextState& state, bool ignoreCharSpacing) const
{
    // Ignore failures
    double length;
    if (!TryGetCharLength(codePoint, state, ignoreCharSpacing, length))
        return GetDefaultCharLength(state, ignoreCharSpacing);

    return length;
}

bool PdfFont::TryGetCharLength(char32_t codePoint, const PdfTextState& state, double& length) const
{
    return TryGetCharLength(codePoint, state, false, length);
}

bool PdfFont::TryGetCharLength(char32_t codePoint, const PdfTextState& state,
    bool ignoreCharSpacing, double& length) const
{
    unsigned gid;
    if (TryGetGID(codePoint, PdfGlyphAccess::Width, gid))
    {
        length = getGlyphLength(m_Metrics->GetGlyphWidth(gid), state, ignoreCharSpacing);
        return true;
    }
    else
    {
        length = getGlyphLength(m_Metrics->GetDefaultWidth(), state, ignoreCharSpacing);
        return false;
    }
}

double PdfFont::GetDefaultCharLength(const PdfTextState& state, bool ignoreCharSpacing) const
{
    if (ignoreCharSpacing)
    {
        return m_Metrics->GetDefaultWidth() * state.FontSize
            * state.FontScale;
    }
    else
    {
        return (m_Metrics->GetDefaultWidth() * state.FontSize
            + state.CharSpacing) * state.FontScale;
    }
}

/*
vector<PdfSplittedString> PdfFont::SplitEncodedString(const PdfString& str) const
{
    (void)str;
    // TODO: retrieve space character codes with m_Encoding->GetToUnicodeMapSafe().TryGetCharCode(codePoint, codeUnit),
    // then iterate char codes and return splitted strings
    PODOFO_RAISE_ERROR(PdfErrorCode::NotImplemented);
}
*/

double PdfFont::GetCIDLengthRaw(unsigned cid) const
{
    unsigned gid;
    if (!TryMapCIDToGID(cid, PdfGlyphAccess::Width, gid))
        return m_Metrics->GetDefaultWidth();

    return m_Metrics->GetGlyphWidth(gid);
}

void PdfFont::GetBoundingBox(PdfArray& arr) const
{
    auto& matrix = m_Metrics->GetMatrix();
    arr.Clear();
    vector<double> bbox;
    m_Metrics->GetBoundingBox(bbox);
    arr.Add(PdfObject(static_cast<int64_t>(std::round(bbox[0] / matrix[0]))));
    arr.Add(PdfObject(static_cast<int64_t>(std::round(bbox[1] / matrix[3]))));
    arr.Add(PdfObject(static_cast<int64_t>(std::round(bbox[2] / matrix[0]))));
    arr.Add(PdfObject(static_cast<int64_t>(std::round(bbox[3] / matrix[3]))));
}

void PdfFont::FillDescriptor(PdfDictionary& dict) const
{
    // Optional values
    int weight;
    double xHeight;
    double stemH;
    string familyName;
    double leading;
    double avgWidth;
    double maxWidth;
    double defaultWidth;
    PdfFontStretch stretch;

    dict.AddKey("FontName"_n, PdfName(this->GetName()));
    if ((familyName = m_Metrics->GetFontFamilyName()).length() != 0)
        dict.AddKey("FontFamily"_n, PdfString(familyName));
    if ((stretch = m_Metrics->GetFontStretch()) != PdfFontStretch::Unknown)
        dict.AddKey("FontStretch"_n, PdfName(toString(stretch)));
    dict.AddKey("Flags"_n, static_cast<int64_t>(m_Metrics->GetFlags()));
    dict.AddKey("ItalicAngle"_n, static_cast<int64_t>(std::round(m_Metrics->GetItalicAngle())));

    auto& matrix = m_Metrics->GetMatrix();
    if (GetType() == PdfFontType::Type3)
    {
        // ISO 32000-1:2008 "should be used for Type 3 fonts in Tagged PDF documents"
        dict.AddKey("FontWeight"_n, static_cast<int64_t>(m_Metrics->GetWeight()));
    }
    else
    {
        if ((weight = m_Metrics->GetWeightRaw()) > 0)
            dict.AddKey("FontWeight"_n, static_cast<int64_t>(weight));

        PdfArray bbox;
        GetBoundingBox(bbox);

        // The following entries are all optional in /Type3 fonts
        dict.AddKey("FontBBox"_n, std::move(bbox));
        dict.AddKey("Ascent"_n, static_cast<int64_t>(std::round(m_Metrics->GetAscent() / matrix[3])));
        dict.AddKey("Descent"_n, static_cast<int64_t>(std::round(m_Metrics->GetDescent() / matrix[3])));
        dict.AddKey("CapHeight"_n, static_cast<int64_t>(std::round(m_Metrics->GetCapHeight() / matrix[3])));
        // NOTE: StemV is measured horizontally
        dict.AddKey("StemV"_n, static_cast<int64_t>(std::round(m_Metrics->GetStemV() / matrix[0])));

        if ((xHeight = m_Metrics->GetXHeightRaw()) > 0)
            dict.AddKey("XHeight"_n, static_cast<int64_t>(std::round(xHeight / matrix[3])));

        if ((stemH = m_Metrics->GetStemHRaw()) > 0)
        {
            // NOTE: StemH is measured vertically
            dict.AddKey("StemH"_n, static_cast<int64_t>(std::round(stemH / matrix[3])));
        }

        if (!IsCIDKeyed())
        {
            // Default for /MissingWidth is 0
            // NOTE: We assume CID keyed fonts to use the /DW entry
            // in the CIDFont dictionary instead. See 9.7.4.3 Glyph
            // Metrics in CIDFonts in ISO 32000-1:2008
            if ((defaultWidth = m_Metrics->GetDefaultWidthRaw()) > 0)
                dict.AddKey("MissingWidth"_n, static_cast<int64_t>(std::round(defaultWidth / matrix[0])));
        }
    }

    if ((leading = m_Metrics->GetLeadingRaw()) > 0)
        dict.AddKey("Leading"_n, static_cast<int64_t>(std::round(leading / matrix[3])));
    if ((avgWidth = m_Metrics->GetAvgWidthRaw()) > 0)
        dict.AddKey("AvgWidth"_n, static_cast<int64_t>(std::round(avgWidth / matrix[0])));
    if ((maxWidth = m_Metrics->GetMaxWidthRaw()) > 0)
        dict.AddKey("MaxWidth"_n, static_cast<int64_t>(std::round(maxWidth / matrix[0])));
}

void PdfFont::EmbedFontFile(PdfObject& descriptor)
{
    auto fontdata = m_Metrics->GetOrLoadFontFileData();
    if (fontdata.empty())
        PODOFO_RAISE_ERROR(PdfErrorCode::InternalLogic);

    switch (m_Metrics->GetFontFileType())
    {
        case PdfFontFileType::Type1:
            EmbedFontFileType1(descriptor, fontdata, m_Metrics->GetFontFileLength1(), m_Metrics->GetFontFileLength2(), m_Metrics->GetFontFileLength3());
            break;
        case PdfFontFileType::Type1CFF:
        case PdfFontFileType::CIDKeyedCFF:
            EmbedFontFileCFF(descriptor, fontdata);
            break;
        case PdfFontFileType::TrueType:
            EmbedFontFileTrueType(descriptor, fontdata);
            break;
        case PdfFontFileType::OpenTypeCFF:
            EmbedFontFileOpenType(descriptor, fontdata);
            break;
        default:
            PODOFO_RAISE_ERROR_INFO(PdfErrorCode::InvalidEnumValue, "Unsupported font type embedding");
    }
}

void PdfFont::EmbedFontFileType1(PdfObject& descriptor, const bufferview& data, unsigned length1, unsigned length2, unsigned length3)
{
    embedFontFileData(descriptor, "FontFile"_n, [length1, length2, length3](PdfDictionary& dict)
    {
        dict.AddKey("Length1"_n, static_cast<int64_t>(length1));
        dict.AddKey("Length2"_n, static_cast<int64_t>(length2));
        dict.AddKey("Length3"_n, static_cast<int64_t>(length3));
    }, data);
}

void PdfFont::EmbedFontFileCFF(PdfObject& descriptor, const bufferview& data)
{
    embedFontFileData(descriptor, "FontFile3"_n, [&](PdfDictionary& dict)
    {
        PdfName subtype;
        if (IsCIDKeyed())
            subtype = "CIDFontType0C"_n;
        else
            subtype = "Type1C"_n;

        dict.AddKey("Subtype"_n, subtype);
    }, data);
}

void PdfFont::EmbedFontFileTrueType(PdfObject& descriptor, const bufferview& data)
{
    embedFontFileData(descriptor, "FontFile2"_n, [&data](PdfDictionary& dict)
    {
        dict.AddKey("Length1"_n, static_cast<int64_t>(data.size()));
    }, data);

}

void PdfFont::EmbedFontFileOpenType(PdfObject& descriptor, const bufferview& data)
{
    embedFontFileData(descriptor, "FontFile3"_n, [](PdfDictionary& dict)
    {
        dict.AddKey("Subtype"_n, "OpenType"_n);
    }, data);
}

void PdfFont::embedFontFileData(PdfObject& descriptor, const PdfName& fontFileName,
    const std::function<void(PdfDictionary& dict)>& dictWriter, const bufferview& data)
{
    auto& contents = GetDocument().GetObjects().CreateDictionaryObject();
    descriptor.GetDictionary().AddKeyIndirect(fontFileName, contents);
    // NOTE: Access to directory is mediated by functor to not crash
    // operations when using PdfStreamedDocument. Do not remove it
    dictWriter(contents.GetDictionary());
    contents.GetOrCreateStream().SetData(data);
}

void PdfFont::initSpaceDescriptors()
{
    if (m_WordSpacingLengthRaw >= 0)
        return;

    // TODO: Maybe try looking up other characters if U' ' is missing?
    // https://docs.microsoft.com/it-it/dotnet/api/system.char.iswhitespace
    unsigned gid;
    if (!TryGetGID(U' ', PdfGlyphAccess::Width, gid)
        || !m_Metrics->TryGetGlyphWidth(gid, m_SpaceCharLengthRaw)
        || m_SpaceCharLengthRaw <= 0)
    {
        double lengthsum = 0;
        unsigned nonZeroCount = 0;
        for (unsigned i = 0, count = m_Metrics->GetGlyphCount(); i < count; i++)
        {
            double length;
            m_Metrics->TryGetGlyphWidth(i, length);
            if (length > 0)
            {
                lengthsum += length;
                nonZeroCount++;
            }
        }

        m_SpaceCharLengthRaw = lengthsum / nonZeroCount;
    }

    // We arbitrarily take a fraction of the read or inferred
    // char space to determine the word spacing length. The
    // factor proved to work well with a consistent tests corpus
    constexpr int WORD_SPACING_FRACTIONAL_FACTOR = 6;
    m_WordSpacingLengthRaw = m_SpaceCharLengthRaw / WORD_SPACING_FRACTIONAL_FACTOR;
}

void PdfFont::initImported()
{
    // By default do nothing
}

double PdfFont::getStringLength(const vector<PdfCID>& cids, const PdfTextState& state) const
{
    double length = 0;
    for (auto& cid : cids)
        length += getGlyphLength(GetCIDLengthRaw(cid.Id), state, false);

    return length;
}

double PdfFont::GetLineSpacing(const PdfTextState& state) const
{
    return m_Metrics->GetLineSpacing() * state.FontSize;
}

// CHECK-ME Should state.GetFontScale() be considered?
double PdfFont::GetUnderlineThickness(const PdfTextState& state) const
{
    return m_Metrics->GetUnderlineThickness() * state.FontSize;
}

// CHECK-ME Should state.GetFontScale() be considered?
double PdfFont::GetUnderlinePosition(const PdfTextState& state) const
{
    return m_Metrics->GetUnderlinePosition() * state.FontSize;
}

// CHECK-ME Should state.GetFontScale() be considered?
double PdfFont::GetStrikeThroughPosition(const PdfTextState& state) const
{
    return m_Metrics->GetStrikeThroughPosition() * state.FontSize;
}

// CHECK-ME Should state.GetFontScale() be considered?
double PdfFont::GetStrikeThroughThickness(const PdfTextState& state) const
{
    return m_Metrics->GetStrikeThroughThickness() * state.FontSize;
}

double PdfFont::GetAscent(const PdfTextState& state) const
{
    return m_Metrics->GetAscent() * state.FontSize;
}

double PdfFont::GetDescent(const PdfTextState& state) const
{
    return m_Metrics->GetDescent() * state.FontSize;
}

PdfCID PdfFont::AddSubsetGIDSafe(unsigned gid, const unicodeview& codePoints)
{
    PODOFO_ASSERT(m_SubsettingEnabled && !m_IsEmbedded);
    if (m_SubstGIDMap == nullptr)
    {
        m_SubstGIDMap.reset(new GIDMap());
    }
    else
    {
        auto found = m_SubstGIDMap->find(gid);
        if (found != m_SubstGIDMap->end())
            return found->second;
    }

    PdfCID ret;
    if (!tryAddSubsetGID(gid, codePoints, ret))
    {
        PODOFO_RAISE_ERROR_INFO(PdfErrorCode::InvalidFontData,
            "The encoding doesn't support these characters or the gid is already present");
    }

    return ret;
}

PdfCharCode PdfFont::AddCharCodeSafe(unsigned gid, const unicodeview& codePoints)
{
    // NOTE: This method is supported only when doing fully embedding
    // of an imported font with valid unicode mapping
    PODOFO_ASSERT(!m_SubsettingEnabled
        && m_Encoding->IsDynamicEncoding()
        && !IsObjectLoaded()
        && m_Metrics->HasUnicodeMapping());

    PdfCharCode code;
    if (m_DynamicToUnicodeMap->TryGetCharCode(codePoints, code))
        return code;

    // Encode the code point with FSS-UTF encoding so
    // it will be variable code size safe
    code = PdfCharCode(utls::FSSUTFEncode((unsigned)m_DynamicToUnicodeMap->GetMappings().size()));
    // NOTE: We assume in this context cid == gid identity
    m_DynamicCIDMap->PushMapping(code, gid);
    m_DynamicToUnicodeMap->PushMapping(code, codePoints);
    return code;
}

bool PdfFont::NeedsCIDMapWriting() const
{
    return m_SubstGIDMap != nullptr;
}

bool PdfFont::tryConvertToGIDs(const std::string_view& utf8Str, PdfGlyphAccess access, std::vector<unsigned>& gids) const
{
    bool success = true;
    if (IsObjectLoaded() || !m_Metrics->HasUnicodeMapping())
    {
        // NOTE: This is a best effort strategy. It's not intended to
        // be accurate in loaded fonts
        auto it = utf8Str.begin();
        auto end = utf8Str.end();

        auto& toUnicode = m_Encoding->GetToUnicodeMapSafe();
        while (it != end)
        {
            char32_t cp = utf8::next(it, end);
            PdfCharCode codeUnit;
            unsigned cid;
            unsigned gid;
            if (toUnicode.TryGetCharCode(cp, codeUnit))
            {
                if (m_Encoding->TryGetCIDId(codeUnit, cid))
                {
                    if (!TryMapCIDToGID(cid, access, gid))
                    {
                        // Fallback
                        gid = cid;
                        success = false;
                    }
                }
                else
                {
                    // Fallback
                    gid = codeUnit.Code;
                    success = false;
                }
            }
            else
            {
                // Fallback
                gid = cp;
                success = false;
            }

            gids.push_back(gid);
        }
    }
    else
    {
        auto it = utf8Str.begin();
        auto end = utf8Str.end();
        while (it != end)
        {
            char32_t cp = utf8::next(it, end);
            unsigned gid;
            if (!m_Metrics->TryGetGID(cp, gid))
            {
                // Fallback
                gid = cp;
                success = false;
            }

            gids.push_back(gid);
        }

        // Try to subsistute GIDs for fonts that support
        // a glyph substitution mechanism
        vector<unsigned char> backwardMap;
        m_Metrics->SubstituteGIDs(gids, backwardMap);
    }

    return success;
}

bool PdfFont::tryAddSubsetGID(unsigned gid, const unicodeview& codePoints, PdfCID& cid)
{
    (void)codePoints;
    PODOFO_ASSERT(m_SubsettingEnabled && !IsObjectLoaded());
    if (m_Encoding->IsDynamicEncoding())
    {
        // We start numberings CIDs from 1 since CID 0
        // is reserved for fallbacks. Encode it with FSS-UTF
        // encoding so it will be variable code size safe
        auto inserted = m_SubstGIDMap->try_emplace(gid, PdfCID((unsigned)m_SubstGIDMap->size() + 1, PdfCharCode(utls::FSSUTFEncode((unsigned)m_SubstGIDMap->size() + 1))));
        cid = inserted.first->second;
        if (!inserted.second)
            return false;

        m_DynamicCIDMap->PushMapping(cid.Unit, cid.Id);
        m_DynamicToUnicodeMap->PushMapping(cid.Unit, codePoints);
        return true;
    }
    else
    {
        PdfCharCode codeUnit;
        if (!m_Encoding->GetToUnicodeMapSafe().TryGetCharCode(codePoints, codeUnit))
        {
            cid = { };
            return false;
        }

        // We start numberings CIDs from 1 since CID 0
        // is reserved for fallbacks
        auto inserted = m_SubstGIDMap->try_emplace(gid, PdfCID((unsigned)m_SubstGIDMap->size() + 1, codeUnit));
        cid = inserted.first->second;
        return inserted.second;
    }
}

void PdfFont::AddSubsetGIDs(const PdfString& encodedStr)
{
    if (IsObjectLoaded())
        PODOFO_RAISE_ERROR_INFO(PdfErrorCode::InternalLogic, "Can't add used GIDs to a loaded font");

    if (m_Encoding->IsDynamicEncoding())
        PODOFO_RAISE_ERROR_INFO(PdfErrorCode::InternalLogic, "Can't add used GIDs from an encoded string to a font with a dynamic encoding");

    if (m_IsEmbedded)
    {
        PODOFO_RAISE_ERROR_INFO(PdfErrorCode::InternalLogic,
            "Can't add more subsetting glyphs on an already embedded font");
    }

    vector<PdfCID> cids;
    unsigned gid;
    (void)GetEncoding().TryConvertToCIDs(encodedStr, cids);
    if (m_SubstGIDMap == nullptr)
        m_SubstGIDMap.reset(new GIDMap());

    for (unsigned i = 0; i < cids.size(); i++)
    {
        auto& cid = cids[i];
        if (TryMapCIDToGID(cid.Id, PdfGlyphAccess::FontProgram, gid))
        {
            if (m_SubsettingEnabled)
            {
                // Ignore trying to replace existing mapping
                (void)m_SubstGIDMap->try_emplace(gid,
                    PdfCID((unsigned)m_SubstGIDMap->size() + 1, cid.Unit));
            }
            else
            {
                if (gid >= m_Metrics->GetGlyphCount())
                {
                    // Assume the font will always contain at least one glyph
                    // and add a mapping to CID 0 for the char code
                    (void)m_SubstGIDMap->try_emplace(0, PdfCID(0, cid.Unit));
                }
                else
                {
                    // Reinsert the cid with actual fetched gid
                    (void)m_SubstGIDMap->try_emplace(gid, PdfCID(gid, cid.Unit));
                }
            }
        }
    }
}

bool PdfFont::SupportsSubsetting() const
{
    return false;
}

bool PdfFont::IsStandard14Font() const
{
    return m_Metrics->IsStandard14FontMetrics();
}

bool PdfFont::IsStandard14Font(PdfStandard14FontType& std14Font) const
{
    return m_Metrics->IsStandard14FontMetrics(std14Font);
}

PdfObject& PdfFont::GetDescendantFontObject()
{
    auto obj = getDescendantFontObject();
    if (obj == nullptr)
        PODOFO_RAISE_ERROR_INFO(PdfErrorCode::InvalidHandle, "Descendant font object must not be null");

    return *obj;
}

bool PdfFont::TryMapCIDToGID(unsigned cid, PdfGlyphAccess access, unsigned& gid) const
{
    if (m_cidToGidMap != nullptr && m_cidToGidMap->HasGlyphAccess(access))
        return m_cidToGidMap->TryMapCIDToGID(cid, gid);

    return tryMapCIDToGID(cid, gid);
}

bool PdfFont::tryMapCIDToGID(unsigned cid, unsigned& gid) const
{
    PODOFO_ASSERT(!IsObjectLoaded());
    if (m_Encoding->IsSimpleEncoding() && m_Metrics->HasUnicodeMapping())
    {
        // Simple encodings must retrieve the gid from the
        // metrics using the mapped unicode code point
        char32_t mappedCodePoint = m_Encoding->GetCodePoint(cid);
        if (mappedCodePoint == U'\0'
            || !m_Metrics->TryGetGID(mappedCodePoint, gid))
        {
            gid = 0;
            return false;
        }

        return true;
    }
    else
    {
        // The font is not loaded, hence it's imported:
        // we assume cid == gid identity. CHECK-ME: Does it work
        // if we font to create a substitute font of a loaded font
        // with a /CIDToGIDMap ???
        gid = cid;
        return true;
    }
}

PdfObject* PdfFont::getDescendantFontObject()
{
    // By default return null
    return nullptr;
}

string_view PdfFont::GetStandard14FontName(PdfStandard14FontType stdFont)
{
    return ::GetStandard14FontName(stdFont);
}

bool PdfFont::IsStandard14Font(const string_view& fontName, PdfStandard14FontType& stdFont)
{
    return ::IsStandard14Font(fontName, true, stdFont);
}

bool PdfFont::IsStandard14Font(const string_view& fontName, bool useAltNames, PdfStandard14FontType& stdFont)
{
    return ::IsStandard14Font(fontName, useAltNames, stdFont);
}

bool PdfFont::IsCIDKeyed() const
{
    switch (GetType())
    {
        case PdfFontType::CIDTrueType:
        case PdfFontType::CIDCFF:
            return true;
        default:
            return false;
    }
}

bool PdfFont::IsObjectLoaded() const
{
    return false;
}

inline string_view PdfFont::GetSubsetPrefix() const
{
    return m_SubsetPrefix;
}

// TODO:
// Handle word spacing Tw
// 5.2.2 Word Spacing
// Note: Word spacing is applied to every occurrence of the single-byte character code
// 32 in a string when using a simple font or a composite font that defines code 32 as a
// single - byte code.It does not apply to occurrences of the byte value 32 in multiplebyte
// codes.
double getGlyphLength(double glyphLength, const PdfTextState& state, bool ignoreCharSpacing)
{
    if (ignoreCharSpacing)
        return glyphLength * state.FontSize * state.FontScale;
    else
        return (glyphLength * state.FontSize + state.CharSpacing) * state.FontScale;
}

string_view toString(PdfFontStretch stretch)
{
    switch (stretch)
    {
        case PdfFontStretch::UltraCondensed:
            return "UltraCondensed";
        case PdfFontStretch::ExtraCondensed:
            return "ExtraCondensed";
        case PdfFontStretch::Condensed:
            return "Condensed";
        case PdfFontStretch::SemiCondensed:
            return "SemiCondensed";
        case PdfFontStretch::Normal:
            return "Normal";
        case PdfFontStretch::SemiExpanded:
            return "SemiExpanded";
        case PdfFontStretch::Expanded:
            return "Expanded";
        case PdfFontStretch::ExtraExpanded:
            return "ExtraExpanded";
        case PdfFontStretch::UltraExpanded:
            return "UltraExpanded";
        default:
            PODOFO_RAISE_ERROR(PdfErrorCode::InvalidEnumValue);
    }
}
