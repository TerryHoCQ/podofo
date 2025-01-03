/**
 * SPDX-FileCopyrightText: (C) 2007 Dominik Seichter <domseichter@web.de>
 * SPDX-FileCopyrightText: (C) 2020 Francesco Pretto <ceztko@gmail.com>
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <podofo/private/PdfDeclarationsPrivate.h>
#include "PdfFontCIDTrueType.h"

#include <podofo/private/FreetypePrivate.h>

#include "PdfDictionary.h"
#include "PdfDocument.h"
#include <podofo/private/FontTrueTypeSubset.h>

using namespace std;
using namespace PoDoFo;

PdfFontCIDTrueType::PdfFontCIDTrueType(PdfDocument& doc, const PdfFontMetricsConstPtr& metrics,
        const PdfEncoding& encoding) : PdfFontCID(doc, metrics, encoding) { }

PdfFontType PdfFontCIDTrueType::GetType() const
{
    return PdfFontType::CIDTrueType;
}

void PdfFontCIDTrueType::embedFontSubset()
{
    auto infos = GetCharGIDInfos();
    createWidths(GetDescendantFont().GetDictionary(), infos);
    m_Encoding->ExportToFont(*this);

    charbuff buffer;
    FontTrueTypeSubset::BuildFont(buffer, GetMetrics(), infos);
    EmbedFontFileTrueType(GetDescriptor(), buffer);

    auto pdfaLevel = GetDocument().GetMetadata().GetPdfALevel();
    if (pdfaLevel == PdfALevel::L1A || pdfaLevel == PdfALevel::L1B)
    {
        // We prepare the /CIDSet content now. NOTE: The CIDSet
        // entry is optional and it's actually deprecated in PDF 2.0
        // but it's required for PDF/A-1 compliance in TrueType CID fonts.
        // Newer compliances remove this requirement, but if present
        // it has even sillier requirements
        string cidSetData;
        for (unsigned i = 0; i < infos.size(); i++)
        {
            // ISO 32000-1:2008: Table 124 – Additional font descriptor entries for CIDFonts
            // CIDSet "The stream’s data shall be organized as a table of bits
            // indexed by CID. The bits shall be stored in bytes with the
            // high - order bit first.Each bit shall correspond to a CID.
            // The most significant bit of the first byte shall correspond
            // to CID 0, the next bit to CID 1, and so on"

            constexpr char bits[] = { '\x80', '\x40', '\x20', '\x10', '\x08', '\x04', '\x02', '\x01' };
            auto& info = infos[i];
            unsigned cid = info.Cid;
            unsigned dataIndex = cid >> 3;
            if (cidSetData.size() < dataIndex + 1)
                cidSetData.resize(dataIndex + 1);

            cidSetData[dataIndex] |= bits[cid & 7];
        }

        auto& cidSetObj = this->GetObject().GetDocument()->GetObjects().CreateDictionaryObject();
        cidSetObj.GetOrCreateStream().SetData(cidSetData);
        GetDescriptor().GetDictionary().AddKeyIndirect("CIDSet"_n, cidSetObj);
    }
}
