/**
 * SPDX-FileCopyrightText: (C) 2022 Francesco Pretto <ceztko@gmail.com>
 * SPDX-License-Identifier: LGPL-2.0-or-later
 * SPDX-License-Identifier: MPL-2.0
 */

#include <podofo/private/PdfDeclarationsPrivate.h>
#include "podofo/private/OpenSSLInternal.h"
#include "PdfCommon.h"
#include "PdfFontManager.h"

using namespace std;
using namespace PoDoFo;

#ifdef DEBUG
PODOFO_EXPORT PdfLogSeverity s_MaxLogSeverity = PdfLogSeverity::Debug;
#else
PODOFO_EXPORT PdfLogSeverity s_MaxLogSeverity = PdfLogSeverity::Information;
#endif // DEBUG

PODOFO_EXPORT LogMessageCallback s_LogMessageCallback;

PODOFO_EXPORT ssl::OpenSSLMain s_SSL;

static unsigned s_MaxObjectCount = (1U << 23) - 1;

void ssl::Init()
{
    // Initialize the OpenSSL singleton
    static struct InitOpenSSL
    {
        InitOpenSSL()
        {
            s_SSL.Init();
        }
    } s_init;
}

void PdfCommon::AddFontDirectory(const string_view& path)
{
    PdfFontManager::AddFontDirectory(path);
}

void PdfCommon::SetLogMessageCallback(const LogMessageCallback& logMessageCallback)
{
    s_LogMessageCallback = logMessageCallback;
}

void PdfCommon::SetMaxLoggingSeverity(PdfLogSeverity logSeverity)
{
    s_MaxLogSeverity = logSeverity;
}

PdfLogSeverity PdfCommon::GetMaxLoggingSeverity()
{
    return s_MaxLogSeverity;
}

bool PdfCommon::IsLoggingSeverityEnabled(PdfLogSeverity logSeverity)
{
    return logSeverity <= s_MaxLogSeverity;
}

unsigned PdfCommon::GetMaxObjectCount()
{
    return s_MaxObjectCount;
}

void PdfCommon::SetMaxObjectCount(unsigned maxObjectCount)
{
    s_MaxObjectCount = maxObjectCount;
}
