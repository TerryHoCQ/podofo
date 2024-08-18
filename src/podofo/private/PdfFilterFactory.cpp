/**
 * SPDX-FileCopyrightText: (C) 2006 Dominik Seichter <domseichter@web.de>
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "PdfDeclarationsPrivate.h"
#include "PdfFilterFactory.h"

#include "PdfFiltersImpl.h"

#include <podofo/main/PdfDictionary.h>
#include <podofo/main/PdfArray.h>

using namespace std;
using namespace PoDoFo;

// An OutputStream class that actually perform the encoding
class PdfFilteredEncodeStream : public OutputStream
{
private:
    void init(OutputStream& outputStream, PdfFilterType filterType)
    {
        m_filter = PdfFilterFactory::Create(filterType);
        m_filter->BeginEncode(outputStream);
    }
    ~PdfFilteredEncodeStream()
    {
        m_filter->EndEncode();
    }
public:
    PdfFilteredEncodeStream(const shared_ptr<OutputStream>& outputStream, PdfFilterType filterType)
        : m_OutputStream(outputStream)
    {
        init(*outputStream, filterType);
    }
protected:
    void writeBuffer(const char* buffer, size_t len) override
    {
        m_filter->EncodeBlock({ buffer, len });
    }
private:
    shared_ptr<OutputStream> m_OutputStream;
    unique_ptr<PdfFilter> m_filter;
};

// An OutputStream class that actually perform the deecoding
class PdfFilteredDecodeStream : public OutputStream
{
private:
    void init(OutputStream& outputStream, const PdfFilterType filterType,
        const PdfDictionary* decodeParms)
    {
        m_filter = PdfFilterFactory::Create(filterType);
        m_filter->BeginDecode(outputStream, decodeParms);
    }

public:
    PdfFilteredDecodeStream(OutputStream& outputStream, const PdfFilterType filterType,
        const PdfDictionary* decodeParms)
        : m_FilterFailed(false)
    {
        init(outputStream, filterType, decodeParms);
    }

    PdfFilteredDecodeStream(unique_ptr<OutputStream> outputStream, const PdfFilterType filterType,
        const PdfDictionary* decodeParms)
        : m_OutputStream(std::move(outputStream)), m_FilterFailed(false)
    {
        if (m_OutputStream == nullptr)
            PODOFO_RAISE_ERROR_INFO(PdfErrorCode::InvalidHandle, "Output stream must be not null");

        init(*m_OutputStream, filterType, decodeParms);
    }

protected:
    void writeBuffer(const char* buffer, size_t len) override
    {
        try
        {
            m_filter->DecodeBlock({ buffer, len });
        }
        catch (PdfError& e)
        {
            PODOFO_PUSH_FRAME(e);
            m_FilterFailed = true;
            throw;
        }
    }
    void flush() override
    {
        try
        {
            if (!m_FilterFailed)
                m_filter->EndDecode();
        }
        catch (PdfError& e)
        {
            PODOFO_PUSH_FRAME_INFO(e, "PdfFilter::EndDecode() failed in filter of type {}",
                PoDoFo::FilterToName(m_filter->GetType()));
            m_FilterFailed = true;
            throw;
        }
    }

private:
    shared_ptr<OutputStream> m_OutputStream;
    unique_ptr<PdfFilter> m_filter;
    bool m_FilterFailed;
};

// An InputStream class that will actually perform the decoding
class PdfBufferedDecodeStream : public InputStream, private OutputStream
{
public:
    PdfBufferedDecodeStream(const shared_ptr<InputStream>& inputStream, const PdfFilterList& filters,
        const vector<const PdfDictionary*>& decodeParms)
        : m_inputEof(false), m_inputStream(inputStream), m_offset(0)
    {
        PODOFO_INVARIANT(filters.size() != 0);
        int i = (int)filters.size() - 1;
        m_filterStream.reset(new PdfFilteredDecodeStream(*this, filters[i], decodeParms[i]));
        i--;

        while (i >= 0)
        {
            m_filterStream.reset(new PdfFilteredDecodeStream(std::move(m_filterStream), filters[i], decodeParms[i]));
            i--;
        }
    }
protected:
    size_t readBuffer(char* buffer, size_t size, bool& eof) override
    {
        if (m_offset < m_buffer.size())
        {
            size = std::min(size, m_buffer.size() - m_offset);
            std::memcpy(buffer, m_buffer.data() + m_offset, size);
            m_offset += size;
            eof = false;
            return size;
        }

        if (m_inputEof)
        {
            eof = true;
            return 0;
        }

        auto readSize = ReadBuffer(*m_inputStream, buffer, size, m_inputEof);
        m_buffer.clear();
        m_filterStream->Write(buffer, readSize);
        if (m_inputEof)
            m_filterStream->Flush();

        size = std::min(size, m_buffer.size());
        std::memcpy(buffer, m_buffer.data(), size);
        m_offset = size;
        eof = false;
        return size;
    }

    void writeBuffer(const char* buffer, size_t size) override
    {
        m_buffer.append(buffer, size);
    }
private:
    bool m_inputEof;
    shared_ptr<InputStream> m_inputStream;
    size_t m_offset;
    charbuff m_buffer;
    unique_ptr<OutputStream> m_filterStream;
};

unique_ptr<PdfFilter> PdfFilterFactory::Create(PdfFilterType filterType)
{
    unique_ptr<PdfFilter> ret;
    if (!TryCreate(filterType, ret))
        PODOFO_RAISE_ERROR(PdfErrorCode::UnsupportedFilter);

    return ret;
}

bool PdfFilterFactory::TryCreate(PdfFilterType filterType, unique_ptr<PdfFilter>& filter)
{
    switch (filterType)
    {
        case PdfFilterType::ASCIIHexDecode:
            filter = unique_ptr<PdfFilter>(new PdfHexFilter());
            return true;
        case PdfFilterType::ASCII85Decode:
            filter = unique_ptr<PdfFilter>(new PdfAscii85Filter());
            return true;
        case PdfFilterType::LZWDecode:
            filter = unique_ptr<PdfFilter>(new PdfLZWFilter());
            return true;
        case PdfFilterType::FlateDecode:
            filter = unique_ptr<PdfFilter>(new PdfFlateFilter());
            return true;
        case PdfFilterType::RunLengthDecode:
            filter = unique_ptr<PdfFilter>(new PdfRLEFilter());
            return true;
        case PdfFilterType::Crypt:
            filter = unique_ptr<PdfFilter>(new PdfCryptFilter());
            return true;
        case PdfFilterType::None:
        case PdfFilterType::DCTDecode:
        case PdfFilterType::CCITTFaxDecode:
        case PdfFilterType::JBIG2Decode:
        case PdfFilterType::JPXDecode:
        default:
            return false;
    }
}

unique_ptr<OutputStream> PdfFilterFactory::CreateEncodeStream(const shared_ptr<OutputStream>& stream,
    const PdfFilterList& filters)
{
    PODOFO_RAISE_LOGIC_IF(!filters.size(), "Cannot create an EncodeStream from an empty list of filters");

    PdfFilterList::const_iterator it = filters.begin();
    unique_ptr<OutputStream> filter(new PdfFilteredEncodeStream(stream, *it));
    it++;

    while (it != filters.end())
    {
        filter.reset(new PdfFilteredEncodeStream(std::move(filter), *it));
        it++;
    }

    return filter;
}

unique_ptr<InputStream> PdfFilterFactory::CreateDecodeStream(const shared_ptr<InputStream>& stream,
    const PdfFilterList& filters, const std::vector<const PdfDictionary*>& decodeParms)
{
    PODOFO_RAISE_LOGIC_IF(stream == nullptr, "Cannot create an DecodeStream from an empty stream");
    PODOFO_RAISE_LOGIC_IF(filters.size() == 0, "Cannot create an DecodeStream from an empty list of filters");
    return std::make_unique<PdfBufferedDecodeStream>(stream, filters, decodeParms);
}

PdfFilterList PdfFilterFactory::CreateFilterList(const PdfObject& filtersObj)
{
    PdfFilterList filters;
    const PdfObject* filterKeyObj = nullptr;
    if (filtersObj.IsDictionary()
        && (filterKeyObj = filtersObj.GetDictionary().FindKey("Filter")) == nullptr
        && filtersObj.IsArray())
    {
        filterKeyObj = &filtersObj;
    }
    else if (filtersObj.IsName())
    {
        filterKeyObj = &filtersObj;
    }

    if (filterKeyObj == nullptr)
    {
        // Object had no /Filter key . Return a null filter list.
        return filters;
    }

    if (filterKeyObj->IsName())
    {
        addFilterTo(filters, filterKeyObj->GetName().GetString());
    }
    else if (filterKeyObj->IsArray())
    {
        for (auto filter : filterKeyObj->GetArray().GetIndirectIterator())
        {
            if (!filter->IsName())
                PODOFO_RAISE_ERROR_INFO(PdfErrorCode::UnsupportedFilter, "Filter array contained unexpected non-name type");

            addFilterTo(filters, filter->GetName().GetString());
        }
    }
    else
    {
        PODOFO_RAISE_ERROR_INFO(PdfErrorCode::InvalidDataType, "Unexpected filter container type");
    }

    return filters;
}


void PdfFilterFactory::addFilterTo(PdfFilterList& filters, const string_view& filter)
{
    auto type = PoDoFo::NameToFilter(filter, true);
    filters.push_back(type);
}
