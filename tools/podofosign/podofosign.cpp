/**
 * SPDX-FileCopyrightText: (C) 2016 zyx <zyx@litePDF.cz>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <podofo/private/PdfDeclarationsPrivate.h>
#include <podofo/podofo.h>

#include <cstdlib>
#include <cstdio>
#include <string>
#include <iostream>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#if defined(_WIN64)
#define fseeko _fseeki64
#define ftello _ftelli64
#else
#define fseeko fseek
#define ftello ftell
#endif

using namespace std;
using namespace PoDoFo;

class MySigner : public PdfSigner
{
public:
    MySigner(X509* cert, EVP_PKEY* pkey, const EVP_MD* digest)
        : m_cert(cert), m_pkey(pkey), m_digest(digest) { }

protected:
    void Reset() override
    {
        m_buffer.clear();
    }

    void AppendData(const bufferview& data) override
    {
        m_buffer.append(data.data(), data.size());
    }

    void ComputeSignature(charbuff& buffer, bool dryrun) override;

    /**
     * Should return the signature /Filter, for example "Adobe.PPKLite"
     */
    string GetSignatureFilter() const override
    {
        return "Adobe.PPKLite";
    }

    /**
     * Should return the signature /SubFilter, for example "ETSI.CAdES.detached"
     */
    string GetSignatureSubFilter() const override
    {
        return "adbe.pkcs7.detached";
    }

    string GetSignatureType() const override
    {
        return "Sig";
    }

private:
    charbuff m_buffer;
    X509* m_cert;
    EVP_PKEY* m_pkey;
    const EVP_MD* m_digest;
};

static int print_errors_string(const char* str, size_t len, void* u)
{
    string* pstr = reinterpret_cast<string*>(u);

    if (!pstr || !len || !str)
        return 0;

    if (!pstr->empty() && (*pstr)[pstr->length() - 1] != '\n')
        *pstr += "\n";

    *pstr += string(str, len);

    // to continue
    return 1;
}

static void raise_podofo_error_with_opensslerror(const char* detail)
{
    string err;

    ERR_print_errors_cb(print_errors_string, &err);

    if (err.empty())
        err = "Unknown OpenSSL error";

    err = ": " + err;
    err = detail + err;

    PODOFO_RAISE_ERROR_INFO(PdfErrorCode::InvalidHandle, err.c_str());
}

static int pkey_password_cb(char* buf, int bufsize, int rwflag, void* userdata)
{
    (void)rwflag;
    const char* password = reinterpret_cast<const char*>(userdata);

    if (!password)
        return 0;

    int res = strlen(password);

    if (res > bufsize)
        res = bufsize;

    memcpy(buf, password, res);

    return res;
}

static bool load_cert_and_key(const char* certfile, const char* pkeyfile, const char* pkey_password, X509** out_cert, EVP_PKEY** out_pkey, int32_t& min_signature_size)
{
    min_signature_size = 0;

    if (!certfile || !*certfile)
    {
        cerr << "Certificate file not specified" << endl;
        return false;
    }

    if (!pkeyfile || !*pkeyfile)
    {
        cerr << "Private key file not specified" << endl;
        return false;
    }

    // should not happen, but let's be paranoid
    if (!out_cert || !out_pkey)
    {
        cerr << "Invalid call of load_cert_and_key" << endl;
        return false;
    }

    FILE* fp;

    fp = fopen(certfile, "rb");

    if (!fp)
    {
        cerr << "Failed to open certificate file '" << certfile << "'" << endl;
        return false;
    }

    *out_cert = PEM_read_X509(fp, NULL, NULL, NULL);

    if (fseeko(fp, 0, SEEK_END) != -1)
        min_signature_size += ftello(fp);
    else
        min_signature_size += 3072;

    fclose(fp);

    if (!*out_cert)
    {
        cerr << "Failed to decode certificate file '" << certfile << "'" << endl;
        string err;

        ERR_print_errors_cb(print_errors_string, &err);

        if (!err.empty())
            cerr << err.c_str() << endl;

        return false;
    }

    fp = fopen(pkeyfile, "rb");

    if (!fp)
    {
        X509_free(*out_cert);
        *out_cert = NULL;

        cerr << "Failed to private key file '" << pkeyfile << "'" << endl;
        return false;
    }

    *out_pkey = PEM_read_PrivateKey(fp, NULL, pkey_password_cb, const_cast<char*>(pkey_password));

    if (fseeko(fp, 0, SEEK_END) != -1)
        min_signature_size += ftello(fp);
    else
        min_signature_size += 1024;

    fclose(fp);

    if (!*out_pkey)
    {
        X509_free(*out_cert);
        *out_cert = NULL;

        cerr << "Failed to decode private key file '" << pkeyfile << "'" << endl;
        string err;

        ERR_print_errors_cb(print_errors_string, &err);

        if (!err.empty())
            cerr << err.c_str() << endl;

        return false;
    }

    return true;
}

static void print_help(bool bOnlyUsage)
{
    if (!bOnlyUsage)
    {
        cout << "Digitally signs existing PDF file with the given certificate and private key." << endl;
    }

    cout << endl;
    cout << "Usage: podofosign [arguments]" << endl;
    cout << "The required arguments:" << endl;
    cout << "  -in [inputfile] ... an input file to sign; if no -out is set, updates the input file" << endl;
    cout << "  -cert [certfile] ... a file with a PEM-encoded certificate to include in the document" << endl;
    cout << "  -pkey [pkeyfile] ... a file with a PEM-encoded private key to sign the document with" << endl;
    cout << "The optional arguments:" << endl;
    cout << "  -out [outputfile] ... an output file to save the signed document to; cannot be the same as the input file" << endl;
    cout << "  -password [password] ... a password to unlock the private key file" << endl;
    cout << "  -digest [name] ... a digest name to use for the signature; default is SHA512" << endl;
    cout << "  -reason [utf8-string] ... a UTF-8 encoded string with the reason of the signature; default reason is \"I agree\"" << endl;
    cout << "  -sigsize [size] ... how many bytes to allocate for the signature; the default is derived from the certificate and private key file size" << endl;
    cout << "  -field-name [name] ... field name to use; defaults to 'PoDoFoSignatureFieldXXX', where XXX is the object number" << endl;
    cout << "  -field-use-existing ... whether to use existing signature field, if such named exists; the field type should be a signature" << endl;
    cout << "  -annot-units [mm|inch] ... set units for the annotation positions; default is mm" << endl;
    cout << "  -annot-position [page,left,top,width,height] ... where to place the annotation" << endl;
    cout << "       page ... a 1-based page index (integer), where '1' means the first page, '2' the second, and so on" << endl;
    cout << "       left,top,width,height ... a rectangle (in annot-units) where to place the annotation on the page (double)" << endl;
    cout << "  -annot-print ... use that to have the annotation printable, otherwise it's not printed (the default is not to print it)" << endl;
    cout << "  -annot-font [size,rrggbb,name] ... sets a font for the following annot-text; default is \"5,000000,Helvetica\" in mm" << endl;
    cout << "       size ... the font size, in annot-units" << endl;
    cout << "       rrggbb ... the font color, where rr is for red, gg for green and bb for blue, all two-digit hexa values between 00 and ff" << endl;
    cout << "       name ... the font name to use; if a Base14 font is recognized, then it is used, instead of embedding a new font" << endl;
    cout << "  -annot-text [left,top,utf8-string] ... a UTF-8 encoded string to add to the annotation" << endl;
    cout << "       left,top ... the position (in annot-units, relative to annot-position) where to place the text (double)" << endl;
    cout << "       text ... the actual UTF-8 encoded string to add to the annotation" << endl;
    cout << "  -annot-image [left,top,width,height,filename] ... an image to add to the annotation" << endl;
    cout << "       left,top,width,height ... a rectangle (in annot-units) where to place the image (double), relative to annot-position" << endl;
    cout << "       filename ... a filname of the image to add" << endl;
    cout << "The annotation arguments can be repeated, except of the -annot-position and -annot-print, which can appear up to once." << endl;
    cout << "The -annot-print, -annot-font, -annot-text and -annot-image can appear only after -annot-position." << endl;
    cout << "All the left,top positions are treated with 0,0 being at the left-top of the page." << endl;
    cout << "No drawing is done when using existing field." << endl;
}

static double convert_to_pdf_units(const char* annot_units, double value)
{
    if (strcmp(annot_units, "mm") == 0)
    {
        return 72.0 * value / 25.4;
    }
    else if (strcmp(annot_units, "inch") == 0)
    {
        return 72.0 * value;
    }
    else
    {
        string err = "Unknown annotation unit '";
        err += annot_units;
        err += "'";

        PODOFO_RAISE_ERROR_INFO(PdfErrorCode::InvalidEnumValue, err);
    }
}

static bool parse_annot_position(const char* annot_position,
    const char* annot_units,
    int& annot_page,
    double& annot_left,
    double& annot_top,
    double& annot_width,
    double& annot_height)
{
    float fLeft, fTop, fWidth, fHeight;

    if (sscanf(annot_position, "%d,%f,%f,%f,%f", &annot_page, &fLeft, &fTop, &fWidth, &fHeight) != 5)
    {
        return false;
    }

    annot_left = convert_to_pdf_units(annot_units, fLeft);
    annot_top = convert_to_pdf_units(annot_units, fTop);
    annot_width = convert_to_pdf_units(annot_units, fWidth);
    annot_height = convert_to_pdf_units(annot_units, fHeight);

    if (annot_page < 1)
        return false;

    annot_page--;

    return true;
}

static const char* skip_commas(const char* text, int ncommas)
{
    if (!text)
    {
        PODOFO_RAISE_ERROR(PdfErrorCode::InvalidHandle);
    }

    const char* res = text;

    while (*res && ncommas > 0)
    {
        if (*res == ',')
            ncommas--;

        res++;
    }

    if (ncommas > 0)
    {
        string err = "The text '";
        err += text;
        err += "' does not conform to the specified format (no enougt commas)";

        PODOFO_RAISE_ERROR_INFO(PdfErrorCode::InvalidDataType, err.c_str());
    }

    return res;
}

static void draw_annotation(PdfDocument& document,
    PdfPainter& painter,
    int argc,
    char* argv[],
    const PdfRect& annot_rect)
{
    const char* annot_units = "mm";
    double font_size = convert_to_pdf_units("mm", 5.0);
    PdfColor font_color(0.0, 0.0, 0.0);
    const char* font_name = "Helvetica";
    bool updateFont = true;
    int ii;

    for (ii = 1; ii < argc; ii++)
    {
        if (strcmp(argv[ii], "-annot-units") == 0)
        {
            annot_units = argv[ii + 1];
        }
        else if (strcmp(argv[ii], "-annot-font") == 0)
        {
            float fSize;
            int rr, gg, bb;

            if (sscanf(argv[ii + 1], "%f,%02x%02x%02x,", &fSize, &rr, &gg, &bb) != 4)
            {
                string err = "The value for -annot-font '";
                err += argv[ii + 1];
                err += "' doesn't conform to format 'size,rrggbb,name'";

                PODOFO_RAISE_ERROR_INFO(PdfErrorCode::InvalidDataType, err.c_str());
            }

            font_size = convert_to_pdf_units(annot_units, fSize);
            font_color = PdfColor(static_cast<double>(rr) / 255.0, static_cast<double>(gg) / 255.0, static_cast<double>(bb) / 255.0);
            font_name = skip_commas(argv[ii + 1], 2);
            updateFont = true;
        }
        else if (strcmp(argv[ii], "-annot-text") == 0)
        {
            float left, top;

            if (sscanf(argv[ii + 1], "%f,%f,", &left, &top) != 2)
            {
                string err = "The value for -annot-text '";
                err += argv[ii + 1];
                err += "' doesn't conform to format 'left,top,text'";

                PODOFO_RAISE_ERROR_INFO(PdfErrorCode::InvalidDataType, err.c_str());
            }

            const char* text = skip_commas(argv[ii + 1], 2);

            if (updateFont)
            {
                PdfFont* font;

                font = document.GetFonts().SearchFont(font_name);
                if (!font)
                {
                    string err = "Failed to create font '";
                    err += font_name;
                    err += "'";

                    PODOFO_RAISE_ERROR_INFO(PdfErrorCode::OutOfMemory, err.c_str());
                }

                painter.GetTextState().SetFont(*font, font_size);
                painter.GetGraphicsState().SetStrokeColor(font_color);
            }

            left = convert_to_pdf_units(annot_units, left);
            top = convert_to_pdf_units(annot_units, top);

            painter.DrawTextMultiLine(text, left,
                0.0,
                annot_rect.GetWidth() - left,
                annot_rect.GetHeight() - top);
        }
        else if (strcmp(argv[ii], "-annot-image") == 0)
        {
            float left, top, width, height;

            if (sscanf(argv[ii + 1], "%f,%f,%f,%f,", &left, &top, &width, &height) != 4)
            {
                string err = "The value for -annot-image '";
                err += argv[ii + 1];
                err += "' doesn't conform to format 'left,top,width,height,filename'";

                PODOFO_RAISE_ERROR_INFO(PdfErrorCode::InvalidDataType, err.c_str());
            }

            const char* filename = skip_commas(argv[ii + 1], 4);

            left = convert_to_pdf_units(annot_units, left);
            top = convert_to_pdf_units(annot_units, top);
            width = convert_to_pdf_units(annot_units, width);
            height = convert_to_pdf_units(annot_units, height);

            auto image = document.CreateImage();
            image->Load(filename);

            double dScaleX = left / image->GetWidth();
            double dScaleY = height / image->GetHeight();

            painter.DrawImage(*image, left, annot_rect.GetHeight() - top - height, dScaleX, dScaleY);
        }

        // these are the only parameters without additional value
        if (strcmp(argv[ii], "-annot-print") != 0 &&
            strcmp(argv[ii], "-field-use-existing") != 0)
            ii++;
    }
}

static PdfObject* find_existing_signature_field(PdfAcroForm& acroForm, const PdfString& name)
{
    PdfObject* fields = acroForm.GetObject().GetDictionary().GetKey("Fields");
    if (fields)
    {
        if (fields->GetDataType() == PdfDataType::Reference)
            fields = acroForm.GetDocument().GetObjects().GetObject(fields->GetReference());

        if (fields && fields->GetDataType() == PdfDataType::Array)
        {
            PdfArray& rArray = fields->GetArray();
            PdfArray::iterator it, end = rArray.end();
            for (it = rArray.begin(); it != end; it++)
            {
                // require references in the Fields array
                if (it->GetDataType() == PdfDataType::Reference)
                {
                    PdfObject* item = acroForm.GetDocument().GetObjects().GetObject(it->GetReference());

                    if (item && item->GetDictionary().HasKey("T") &&
                        item->GetDictionary().GetKey("T")->GetString() == name)
                    {
                        // found a field with the same name
                        const PdfObject* ft = item->GetDictionary().GetKey("FT");
                        if (!ft && item->GetDictionary().HasKey("Parent"))
                        {
                            const PdfObject* temp = item->GetDictionary().FindKey("Parent");
                            if (!temp)
                            {
                                PODOFO_RAISE_ERROR(PdfErrorCode::InvalidDataType);
                            }

                            ft = temp->GetDictionary().GetKey("FT");
                        }

                        if (!ft)
                        {
                            PODOFO_RAISE_ERROR(PdfErrorCode::NoObject);
                        }

                        const PdfName fieldType = ft->GetName();
                        if (fieldType != "Sig")
                        {
                            string err = "Existing field '";
                            err += name.GetString();
                            err += "' isn't of a signature type, but '";
                            err += fieldType.GetString();
                            err += "' instead";

                            PODOFO_RAISE_ERROR_INFO(PdfErrorCode::InvalidName, err.c_str());
                        }

                        return item;
                    }
                }
            }
        }
    }

    return NULL;
}

#if 0 /* TODO */
static void update_default_appearance_streams(PdfAcroForm* pAcroForm)
{
    if (!pAcroForm ||
        !pAcroForm->GetObject()->GetDictionary().HasKey("Fields") ||
        pAcroForm->GetObject()->GetDictionary().GetKey("Fields")->GetDataType() != PdfDataType::Array)
        return;

    PdfArray& rFields = pAcroForm->GetObject()->GetDictionary().GetKey("Fields")->GetArray();

    PdfArray::iterator it, end = rFields.end();
    for (it = rFields.begin(); it != end; it++)
    {
        if (it->GetDataType() == PdfDataType::Reference)
        {
            PdfObject* pObject = pAcroForm->GetDocument()->GetObjects()->GetObject(it->GetReference());
            if (!pObject || pObject->GetDataType() != PdfDataType::Dictionary)
                continue;

            PdfDictionary& rFielDict = pObject->GetDictionary();
            if (rFielDict.HasKey("FT") &&
                rFielDict.GetKey("FT")->GetDataType() == PdfDataType::Name &&
                (rFielDict.GetKey("FT")->GetName() == "Tx" || rFielDict.GetKey("FT")->GetName() == "Ch"))
            {
                PdfString rDA, rV, rDV;

                if (rFielDict.HasKey("V") &&
                    (rFielDict.GetKey("V")->GetDataType() == PdfDataType::String || rFielDict.GetKey("V")->GetDataType() == PdfDataType::HexString))
                {
                    rV = rFielDict.GetKey("V")->GetString();
                }

                if (rFielDict.HasKey("DV") &&
                    (rFielDict.GetKey("DV")->GetDataType() == PdfDataType::String || rFielDict.GetKey("DV")->GetDataType() == PdfDataType::HexString))
                {
                    rDV = rFielDict.GetKey("DV")->GetString();
                }

                if (rV.IsValid() && rV.GetCharacterLength() > 0)
                {
                    rDV = rV;
                }

                if (!rDV.IsValid() || rDV.GetCharacterLength() <= 0)
                    continue;

                if (rDV.GetLength() >= 2 && rDV.GetString()[0] == static_cast<char>(0xFE) && rDV.GetString()[1] == static_cast<char>(0xFF))
                {
                    if (rDV.GetLength() == 2)
                        continue;
                }

                if (rFielDict.HasKey("DA") &&
                    rFielDict.GetKey("DA")->GetDataType() == PdfDataType::String)
                {
                    rDA = rFielDict.GetKey("DA")->GetString();
                }

                if (rFielDict.HasKey("AP") &&
                    rFielDict.GetKey("AP")->GetDataType() == PdfDataType::Dictionary &&
                    rFielDict.GetKey("AP")->GetDictionary().HasKey("N") &&
                    rFielDict.GetKey("AP")->GetDictionary().GetKey("N")->GetDataType() == PdfDataType::Reference)
                {
                    pObject = pAcroForm->GetDocument()->GetObjects()->GetObject(rFielDict.GetKey("AP")->GetDictionary().GetKey("N")->GetReference());
                    if (pObject->GetDataType() == PdfDataType::Dictionary &&
                        pObject->GetDictionary().HasKey("Type") &&
                        pObject->GetDictionary().GetKey("Type")->GetDataType() == PdfDataType::Name &&
                        pObject->GetDictionary().GetKey("Type")->GetName() == "XObject")
                    {
                        PdfXObject xObject(pObject);
                        PdfStream* pCanvas = xObject.GetContentsForAppending()->GetStream();

                        if (rFielDict.GetKey("FT")->GetName() == "Tx")
                        {
                            pCanvas->BeginAppend(true);

                            PdfRefCountedBuffer rBuffer;
                            PdfOutputDevice rOutputDevice(&rBuffer);

                            rDV.Write(&rOutputDevice, ePdfWriteMode_Compact);

                            ostringstream oss;

                            oss << "/Tx BMC" << endl;
                            oss << "BT" << endl;
                            if (rDA.IsValid())
                                oss << rDA.GetString() << endl;
                            oss << "2.0 2.0 Td" << endl;
                            oss << rBuffer.GetBuffer() << " Tj" << endl;
                            oss << "ET" << endl;
                            oss << "EMC" << endl;

                            pCanvas->Append(oss.str());

                            pCanvas->EndAppend();
                        }
                        else if (rFielDict.GetKey("FT")->GetName() == "Ch")
                        {
                        }
                    }
                }
            }
        }
    }
}
#endif

int main(int argc, char* argv[])
{
    const char* inputfile = NULL;
    const char* outputfile = NULL;
    const char* certfile = NULL;
    const char* pkeyfile = NULL;
    const char* password = NULL;
    const char* digest = NULL;
    const char* reason = "I agree";
    const char* sigsizestr = NULL;
    const char* annot_units = "mm";
    const char* annot_position = NULL;
    const char* field_name = NULL;
    int annot_page = 0;
    double annot_left = 0.0, annot_top = 0.0, annot_width = 0.0, annot_height = 0.0;
    bool annot_print = false;
    bool field_use_existing = false;
    int ii;

    PdfCommon::SetMaxLoggingSeverity(PdfLogSeverity::None);

    for (ii = 1; ii < argc; ii++)
    {
        const char** value = NULL;

        if (strcmp(argv[ii], "-in") == 0)
        {
            value = &inputfile;
        }
        else if (strcmp(argv[ii], "-out") == 0)
        {
            value = &outputfile;
        }
        else if (strcmp(argv[ii], "-cert") == 0)
        {
            value = &certfile;
        }
        else if (strcmp(argv[ii], "-pkey") == 0)
        {
            value = &pkeyfile;
        }
        else if (strcmp(argv[ii], "-digest") == 0)
        {
            value = &digest;
        }
        else if (strcmp(argv[ii], "-password") == 0)
        {
            value = &password;
        }
        else if (strcmp(argv[ii], "-reason") == 0)
        {
            value = &reason;
        }
        else if (strcmp(argv[ii], "-sigsize") == 0)
        {
            value = &sigsizestr;
        }
        else if (strcmp(argv[ii], "-annot-units") == 0)
        {
            value = &annot_units;
        }
        else if (strcmp(argv[ii], "-annot-position") == 0)
        {
            if (annot_position)
            {
                cerr << "Only one -annot-position can be specified" << endl;

                return -1;
            }

            value = &annot_position;
        }
        else if (strcmp(argv[ii], "-annot-print") == 0)
        {
            if (!annot_position)
            {
                cerr << "Missing -annot-position argument, which should be defined before '" << argv[ii] << "'" << endl;

                return -2;
            }

            if (annot_print)
            {
                cerr << "Only one -annot-print can be specified" << endl;

                return -1;
            }

            annot_print = !annot_print;
            continue;
        }
        else if (strcmp(argv[ii], "-annot-font") == 0 ||
            strcmp(argv[ii], "-annot-text") == 0 ||
            strcmp(argv[ii], "-annot-image") == 0)
        {
            if (!annot_position)
            {
                cerr << "Missing -annot-position argument, which should be defined before '" << argv[ii] << "'" << endl;

                return -2;
            }
            // value is left NULL, these are parsed later
        }
        else if (strcmp(argv[ii], "-field-name") == 0)
        {
            value = &field_name;
        }
        else if (strcmp(argv[ii], "-field-use-existing") == 0)
        {
            if (field_use_existing)
            {
                cerr << "Only one -field-use-existing can be specified" << endl;

                return -1;
            }

            field_use_existing = !field_use_existing;
            continue;
        }
        else
        {
            cerr << "Unknown argument '" << argv[ii] << "'" << endl;
            print_help(true);

            return -3;
        }

        if (ii + 1 >= argc)
        {
            cerr << "Missing value for argument '" << argv[ii] << "'" << endl;
            print_help(true);

            return -4;
        }

        if (value)
        {
            *value = argv[ii + 1];

            if (*value == annot_units && strcmp(annot_units, "mm") != 0 && strcmp(annot_units, "inch") != 0)
            {
                cerr << "Invalid -annot-units value '" << *value << "', only 'mm' and 'inch' are supported" << endl;

                return -5;
            }

            try {
                if (*value == annot_position && !parse_annot_position(annot_position, annot_units, annot_page, annot_left, annot_top, annot_width, annot_height))
                {
                    cerr << "Invalid -annot-position value '" << *value << "', expected format \"page,left,top,width,height\"" << endl;

                    return -6;
                }
            }
            catch (PdfError& e) {
                cerr << "Invalid -annot-position value '" << *value << "', expected format \"page,left,top,width,height\"" << endl;

                return -6;
            }
        }
        ii++;
    }

    if (!inputfile || !certfile || !pkeyfile)
    {
        if (argc != 1)
            cerr << "Not all required arguments specified." << endl;
        print_help(true);

        return -7;
    }

    int sigsize = -1;

    if (sigsizestr)
    {
        sigsize = atoi(sigsizestr);

        if (sigsize <= 0)
        {
            cerr << "Invalid value for signature size specified (" << sigsizestr << "), use a positive integer, please" << endl;
            return -8;
        }
    }

    if (outputfile && strcmp(outputfile, inputfile) == 0)
    {
        // even I told you not to do it, you still specify the same output file
        // as the input file. Just ignore that.
        outputfile = NULL;
    }

    OPENSSL_init_crypto(0, NULL);

    X509* cert = NULL;
    EVP_PKEY* pkey = NULL;
    int32_t min_signature_size = 0;

    if (!load_cert_and_key(certfile, pkeyfile, password, &cert, &pkey, min_signature_size))
    {
        return -9;
    }

    if (sigsize > 0)
        min_signature_size = sigsize;
    else
        min_signature_size += 1024;

    int result = 0;
    PdfSignature* signature = NULL;

    try
    {
        const EVP_MD* md_digest;

        if (digest != NULL)
        {
            md_digest = EVP_get_digestbyname(digest);
            if (!md_digest)
            {
                string err = "Unknown digest '";
                err += digest;
                err += "'";
                PODOFO_RAISE_ERROR_INFO(PdfErrorCode::InvalidName, err.c_str());
            }
        }
        else
        {
            md_digest = EVP_sha512();
            if (!md_digest)
                cerr << "Cannot get SHA512 digest, using default OpenSSL digest instead." << endl;
        }

        PdfMemDocument document;

        document.Load(inputfile);

        if (!document.GetPages().GetCount())
            PODOFO_RAISE_ERROR_INFO(PdfErrorCode::PageNotFound, "The document has no page. Only documents with at least one page can be signed");

        auto& acroForm = document.GetOrCreateAcroForm();
        if (!acroForm.GetObject().GetDictionary().HasKey("SigFlags") ||
            !acroForm.GetObject().GetDictionary().MustGetKey("SigFlags").IsNumber() ||
            acroForm.GetObject().GetDictionary().FindKeyAsSafe<int64_t>("SigFlags") != 3)
        {
            if (acroForm.GetObject().GetDictionary().HasKey("SigFlags"))
                acroForm.GetObject().GetDictionary().RemoveKey("SigFlags");

            int64_t val = 3;
            acroForm.GetObject().GetDictionary().AddKey("SigFlags", val);
        }

        if (acroForm.GetNeedAppearances())
        {
#if 0 /* TODO */
            update_default_appearance_streams(pAcroForm);
#endif

            acroForm.SetNeedAppearances(false);
        }

        PdfString name;
        PdfObject* existingSigField = NULL;

        if (field_name)
        {
            name = PdfString(field_name);

            existingSigField = find_existing_signature_field(acroForm, name);
            if (existingSigField && !field_use_existing)
            {
                string err = "Signature field named '";
                err += name.GetString();
                err += "' already exists";

                PODOFO_RAISE_ERROR_INFO(PdfErrorCode::WrongDestinationType, err.c_str());
            }
        }
        else
        {
            char fldName[96]; // use bigger buffer to make sure sprintf does not overflow
            sprintf(fldName, "PodofoSignatureField%u", document.GetObjects().GetObjectCount());

            name = PdfString(fldName);
        }

        if (existingSigField)
        {
            if (!existingSigField->GetDictionary().HasKey("P"))
            {
                string err = "Signature field named '";
                err += name.GetString();
                err += "' doesn't have a page reference";

                PODOFO_RAISE_ERROR_INFO(PdfErrorCode::PageNotFound, err.c_str());
            }

            auto& page = document.GetPages().GetPage(existingSigField->GetDictionary().GetKey("P")->GetReference());
            signature = &static_cast<PdfSignature&>(
                static_cast<PdfAnnotationWidget&>(page.GetAnnotations().GetAnnot(existingSigField->GetIndirectReference())).GetField());
            signature->EnsureValueObject();
        }
        else
        {
            auto& page = document.GetPages().GetPageAt(annot_page);
            PdfRect annot_rect;
            if (annot_position)
            {
                annot_rect = PdfRect(annot_left, page.GetMediaBox().GetHeight() - annot_top - annot_height, annot_width, annot_height);
            }

            signature = &page.CreateField<PdfSignature>(name, annot_rect);
            if (annot_position && annot_print)
                signature->MustGetWidget().SetFlags(PdfAnnotationFlags::Print);
            else if (!annot_position && (!field_name || !field_use_existing))
                signature->MustGetWidget().SetFlags(PdfAnnotationFlags::Invisible | PdfAnnotationFlags::Hidden);

            if (annot_position)
            {
                PdfRect annotSize(0.0, 0.0, annot_rect.GetWidth(), annot_rect.GetHeight());
                auto sigXObject = document.CreateXObjectForm(annotSize);
                PdfPainter painter;

                try
                {
                    painter.SetCanvas(*sigXObject);

                    /* Workaround Adobe's reader error 'Expected a dict object.' when the stream
                       contains only one object which does Save()/Restore() on its own, like
                       the image XObject. */
                    painter.Save();
                    painter.Restore();

                    draw_annotation(document, painter, argc, argv, annot_rect);

                    signature->SetAppearanceStream(*sigXObject);
                }
                catch (PdfError& e)
                {
                }

                painter.FinishDrawing();
            }
        }

        signature->SetSignatureReason(PdfString(reason));
        signature->SetSignatureDate(PdfDate());

        MySigner signer(cert, pkey, md_digest);

        FileStreamDevice device(outputfile ? outputfile : inputfile, FileMode::Open, DeviceAccess::Write);

        PoDoFo::SignDocument(document, device, signer, *signature);
    }
    catch (PdfError& e)
    {
        cerr << "Error: An error " << (int)e.GetCode() << " occurred during the sign of the pdf file:" << endl;
        e.PrintErrorMsg();
        result = (int)e.GetCode();
    }

    if (pkey)
        EVP_PKEY_free(pkey);

    if (cert)
        X509_free(cert);

    return result;
}

// TODO: Optmize so the process is buffered
void MySigner::ComputeSignature(charbuff& buffer, bool dryrun)
{
    (void)dryrun;
    int rc;
    BIO* mem = BIO_new(BIO_s_mem());
    if (!mem)
        raise_podofo_error_with_opensslerror("Failed to create input BIO");

    unsigned int flags = PKCS7_DETACHED | PKCS7_BINARY;
    PKCS7* pkcs7 = PKCS7_sign(m_cert, m_pkey, NULL, mem, flags | PKCS7_PARTIAL);
    if (!pkcs7)
    {
        BIO_free(mem);
        raise_podofo_error_with_opensslerror("PKCS7_sign failed");
    }

    if (!PKCS7_sign_add_signer(pkcs7, m_cert, m_pkey, m_digest, 0))
    {
        BIO_free(mem);
        PKCS7_free(pkcs7);
        raise_podofo_error_with_opensslerror("PKCS7_sign_add_signer failed");
    }

    rc = BIO_write(mem, m_buffer.data(), (int)m_buffer.size());
    if (rc != (int)m_buffer.size())
    {
        PKCS7_free(pkcs7);
        BIO_free(mem);
        raise_podofo_error_with_opensslerror("BIO_write failed");
    }

    if (PKCS7_final(pkcs7, mem, flags) <= 0)
    {
        PKCS7_free(pkcs7);
        BIO_free(mem);
        raise_podofo_error_with_opensslerror("PKCS7_final failed");
    }

    BIO* out = BIO_new(BIO_s_mem());
    if (!out)
    {
        PKCS7_free(pkcs7);
        BIO_free(mem);
        raise_podofo_error_with_opensslerror("Failed to create output BIO");
    }

    char* outBuff = NULL;
    long outLen;

    i2d_PKCS7_bio(out, pkcs7);

    outLen = BIO_get_mem_data(out, &outBuff);

    buffer.resize(outLen);
    std::memcpy(buffer.data(), outBuff, outLen);

    PKCS7_free(pkcs7);
    BIO_free(out);
    BIO_free(mem);
}
