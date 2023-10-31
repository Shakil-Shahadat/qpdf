#include <qpdf/QPDFParser.hh>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjGen.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFObject_private.hh>
#include <qpdf/QPDF_Array.hh>
#include <qpdf/QPDF_Bool.hh>
#include <qpdf/QPDF_Dictionary.hh>
#include <qpdf/QPDF_InlineImage.hh>
#include <qpdf/QPDF_Integer.hh>
#include <qpdf/QPDF_Name.hh>
#include <qpdf/QPDF_Null.hh>
#include <qpdf/QPDF_Operator.hh>
#include <qpdf/QPDF_Real.hh>
#include <qpdf/QPDF_Reserved.hh>
#include <qpdf/QPDF_Stream.hh>
#include <qpdf/QPDF_String.hh>
#include <qpdf/QTC.hh>
#include <qpdf/QUtil.hh>

#include <memory>

using ObjectPtr = std::shared_ptr<QPDFObject>;

QPDFObjectHandle
QPDFParser::parse(bool& empty, bool content_stream)
{
    // This method must take care not to resolve any objects. Don't check the type of any object
    // without first ensuring that it is a direct object. Otherwise, doing so may have the side
    // effect of reading the object and changing the file pointer. If you do this, it will cause a
    // logic error to be thrown from QPDF::inParse().

    QPDF::ParseGuard pg(context);
    empty = false;
    start = input->tell();

    if (!tokenizer.nextToken(*input, object_description)) {
        warn(tokenizer.getErrorMessage());
    }

    switch (tokenizer.getType()) {
    case QPDFTokenizer::tt_eof:
        if (content_stream) {
            // In content stream mode, leave object uninitialized to indicate EOF
            return {};
        }
        QTC::TC("qpdf", "QPDFParser eof in parse");
        warn("unexpected EOF");
        return {QPDF_Null::create()};

    case QPDFTokenizer::tt_bad:
        QTC::TC("qpdf", "QPDFParser bad token in parse");
        return {QPDF_Null::create()};

    case QPDFTokenizer::tt_brace_open:
    case QPDFTokenizer::tt_brace_close:
        QTC::TC("qpdf", "QPDFParser bad brace");
        warn("treating unexpected brace token as null");
        return {QPDF_Null::create()};

    case QPDFTokenizer::tt_array_close:
        QTC::TC("qpdf", "QPDFParser bad array close");
        warn("treating unexpected array close token as null");
        return {QPDF_Null::create()};

    case QPDFTokenizer::tt_dict_close:
        QTC::TC("qpdf", "QPDFParser bad dictionary close");
        warn("unexpected dictionary close token");
        return {QPDF_Null::create()};

    case QPDFTokenizer::tt_array_open:
    case QPDFTokenizer::tt_dict_open:
        stack.clear();
        stack.emplace_back(
            input,
            (tokenizer.getType() == QPDFTokenizer::tt_array_open) ? st_array : st_dictionary);
        frame = &stack.back();
        return parseRemainder(content_stream);

    case QPDFTokenizer::tt_bool:
        return withDescription<QPDF_Bool>(tokenizer.getValue() == "true");

    case QPDFTokenizer::tt_null:
        return {QPDF_Null::create()};

    case QPDFTokenizer::tt_integer:
        return withDescription<QPDF_Integer>(QUtil::string_to_ll(tokenizer.getValue().c_str()));

    case QPDFTokenizer::tt_real:
        return withDescription<QPDF_Real>(tokenizer.getValue());

    case QPDFTokenizer::tt_name:
        return withDescription<QPDF_Name>(tokenizer.getValue());

    case QPDFTokenizer::tt_word:
        {
            auto const& value = tokenizer.getValue();
            if (content_stream) {
                return withDescription<QPDF_Operator>(value);
            } else if (value == "endobj") {
                // We just saw endobj without having read anything.  Treat this as a null and do
                // not move the input source's offset.
                input->seek(input->getLastOffset(), SEEK_SET);
                empty = true;
                return {QPDF_Null::create()};
            } else {
                QTC::TC("qpdf", "QPDFParser treat word as string");
                warn("unknown token while reading object; treating as string");
                return withDescription<QPDF_String>(value);
            }
        }

    case QPDFTokenizer::tt_string:
        if (decrypter) {
            std::string s{tokenizer.getValue()};
            decrypter->decryptString(s);
            return withDescription<QPDF_String>(s);
        } else {
            return withDescription<QPDF_String>(tokenizer.getValue());
        }

    default:
        warn("treating unknown token type as null while reading object");
        return {QPDF_Null::create()};
    }
}

QPDFObjectHandle
QPDFParser::parseRemainder(bool content_stream)
{
    // This method must take care not to resolve any objects. Don't check the type of any object
    // without first ensuring that it is a direct object. Otherwise, doing so may have the side
    // effect of reading the object and changing the file pointer. If you do this, it will cause a
    // logic error to be thrown from QPDF::inParse().

    bad_count = 0;
    bool b_contents = false;

    while (true) {
        if (!tokenizer.nextToken(*input, object_description)) {
            warn(tokenizer.getErrorMessage());
        }
        ++good_count; // optimistically

        switch (tokenizer.getType()) {
        case QPDFTokenizer::tt_eof:
            warn("parse error while reading object");
            if (content_stream) {
                // In content stream mode, leave object uninitialized to indicate EOF
                return {};
            }
            QTC::TC("qpdf", "QPDFParser eof in parseRemainder");
            warn("unexpected EOF");
            return {QPDF_Null::create()};

        case QPDFTokenizer::tt_bad:
            QTC::TC("qpdf", "QPDFParser bad token in parseRemainder");
            if (tooManyBadTokens()) {
                return {QPDF_Null::create()};
            }
            addNull();
            continue;

        case QPDFTokenizer::tt_brace_open:
        case QPDFTokenizer::tt_brace_close:
            QTC::TC("qpdf", "QPDFParser bad brace in parseRemainder");
            warn("treating unexpected brace token as null");
            if (tooManyBadTokens()) {
                return {QPDF_Null::create()};
            }
            addNull();
            continue;

        case QPDFTokenizer::tt_array_close:
            if (frame->state == st_array) {
                auto object = QPDF_Array::create(std::move(frame->olist), frame->null_count > 100);
                setDescription(object, frame->offset - 1);
                // The `offset` points to the next of "[".  Set the rewind offset to point to the
                // beginning of "[". This has been explicitly tested with whitespace surrounding the
                // array start delimiter. getLastOffset points to the array end token and therefore
                // can't be used here.
                if (stack.size() <= 1) {
                    return object;
                }
                stack.pop_back();
                frame = &stack.back();
                add(std::move(object));
            } else {
                QTC::TC("qpdf", "QPDFParser bad array close in parseRemainder");
                warn("treating unexpected array close token as null");
                if (tooManyBadTokens()) {
                    return {QPDF_Null::create()};
                }
                addNull();
            }
            continue;

        case QPDFTokenizer::tt_dict_close:
            if (frame->state == st_dictionary) {
                // Convert list to map. Alternating elements are keys.  Attempt to recover more or
                // less gracefully from invalid dictionaries.
                std::set<std::string> names;
                for (auto& obj: frame->olist) {
                    if (obj) {
                        if (obj->getTypeCode() == ::ot_name) {
                            names.insert(obj->getStringValue());
                        }
                    }
                }

                std::map<std::string, QPDFObjectHandle> dict;
                int next_fake_key = 1;
                for (auto iter = frame->olist.begin(); iter != frame->olist.end();) {
                    // Calculate key.
                    std::string key;
                    if (*iter && (*iter)->getTypeCode() == ::ot_name) {
                        key = (*iter)->getStringValue();
                        ++iter;
                    } else {
                        for (bool found_fake = false; !found_fake;) {
                            key = "/QPDFFake" + std::to_string(next_fake_key++);
                            found_fake = (names.count(key) == 0);
                            QTC::TC("qpdf", "QPDFParser found fake", (found_fake ? 0 : 1));
                        }
                        warn(
                            frame->offset,
                            "expected dictionary key but found non-name object; inserting key " +
                                key);
                    }
                    if (dict.count(key) > 0) {
                        QTC::TC("qpdf", "QPDFParser duplicate dict key");
                        warn(
                            frame->offset,
                            "dictionary has duplicated key " + key +
                                "; last occurrence overrides earlier ones");
                    }

                    // Calculate value.
                    ObjectPtr val;
                    if (iter != frame->olist.end()) {
                        val = *iter;
                        ++iter;
                    } else {
                        QTC::TC("qpdf", "QPDFParser no val for last key");
                        warn(
                            frame->offset,
                            "dictionary ended prematurely; using null as value for last key");
                        val = QPDF_Null::create();
                    }

                    dict[std::move(key)] = val;
                }
                if (!frame->contents_string.empty() && dict.count("/Type") &&
                    dict["/Type"].isNameAndEquals("/Sig") && dict.count("/ByteRange") &&
                    dict.count("/Contents") && dict["/Contents"].isString()) {
                    dict["/Contents"] = QPDFObjectHandle::newString(frame->contents_string);
                    dict["/Contents"].setParsedOffset(frame->contents_offset);
                }
                auto object = QPDF_Dictionary::create(std::move(dict));
                setDescription(object, frame->offset - 2);
                // The `offset` points to the next of "<<". Set the rewind offset to point to the
                // beginning of "<<". This has been explicitly tested with whitespace surrounding
                // the dictionary start delimiter. getLastOffset points to the dictionary end token
                // and therefore can't be used here.
                if (stack.size() <= 1) {
                    return object;
                }
                stack.pop_back();
                frame = &stack.back();
                add(std::move(object));
            } else {
                QTC::TC("qpdf", "QPDFParser bad dictionary close in parseRemainder");
                warn("unexpected dictionary close token");
                if (tooManyBadTokens()) {
                    return {QPDF_Null::create()};
                }
                addNull();
            }
            continue;

        case QPDFTokenizer::tt_array_open:
        case QPDFTokenizer::tt_dict_open:
            if (stack.size() > 499) {
                QTC::TC("qpdf", "QPDFParser too deep");
                warn("ignoring excessively deeply nested data structure");
                return {QPDF_Null::create()};
            } else {
                b_contents = false;
                stack.emplace_back(
                    input,
                    (tokenizer.getType() == QPDFTokenizer::tt_array_open) ? st_array
                                                                          : st_dictionary);
                frame = &stack.back();
                continue;
            }

        case QPDFTokenizer::tt_bool:
            addScalar<QPDF_Bool>(tokenizer.getValue() == "true");
            continue;

        case QPDFTokenizer::tt_null:
            addNull();
            continue;

        case QPDFTokenizer::tt_integer:
            addScalar<QPDF_Integer>(QUtil::string_to_ll(tokenizer.getValue().c_str()));
            continue;

        case QPDFTokenizer::tt_real:
            addScalar<QPDF_Real>(tokenizer.getValue());
            continue;

        case QPDFTokenizer::tt_name:
            {
                auto const& name = tokenizer.getValue();
                addScalar<QPDF_Name>(name);

                if (name == "/Contents") {
                    b_contents = true;
                } else {
                    b_contents = false;
                }
            }
            continue;

        case QPDFTokenizer::tt_word:
            {
                auto const& value = tokenizer.getValue();
                auto size = frame->olist.size();
                if (content_stream) {
                    addScalar<QPDF_Operator>(value);
                } else if (
                    value == "R" && size >= 2 && frame->olist.back() &&
                    frame->olist.back()->getTypeCode() == ::ot_integer &&
                    !frame->olist.back()->getObjGen().isIndirect() && frame->olist.at(size - 2) &&
                    frame->olist.at(size - 2)->getTypeCode() == ::ot_integer &&
                    !frame->olist.at(size - 2)->getObjGen().isIndirect()) {
                    if (context == nullptr) {
                        QTC::TC("qpdf", "QPDFParser indirect without context");
                        throw std::logic_error("QPDFObjectHandle::parse called without context on "
                                               "an object with indirect references");
                    }
                    auto ref_og = QPDFObjGen(
                        QPDFObjectHandle(frame->olist.at(size - 2)).getIntValueAsInt(),
                        QPDFObjectHandle(frame->olist.back()).getIntValueAsInt());
                    if (ref_og.isIndirect()) {
                        // This action has the desirable side effect of causing dangling references
                        // (references to indirect objects that don't appear in the PDF) in any
                        // parsed object to appear in the object cache.
                        frame->olist.pop_back();
                        frame->olist.pop_back();
                        add(std::move(context->getObject(ref_og).obj));
                    } else {
                        QTC::TC("qpdf", "QPDFParser indirect with 0 objid");
                        frame->olist.pop_back();
                        frame->olist.pop_back();
                        addNull();
                    }
                } else {
                    QTC::TC("qpdf", "QPDFParser treat word as string in parseRemainder");
                    warn("unknown token while reading object; treating as string");
                    if (tooManyBadTokens()) {
                        return {QPDF_Null::create()};
                    }
                    addScalar<QPDF_String>(value);
                }
            }
            continue;

        case QPDFTokenizer::tt_string:
            {
                auto const& val = tokenizer.getValue();
                if (decrypter) {
                    if (b_contents) {
                        frame->contents_string = val;
                        frame->contents_offset = input->getLastOffset();
                        b_contents = false;
                    }
                    std::string s{val};
                    decrypter->decryptString(s);
                    addScalar<QPDF_String>(s);
                } else {
                    addScalar<QPDF_String>(val);
                }
            }
            continue;

        default:
            warn("treating unknown token type as null while reading object");
            if (tooManyBadTokens()) {
                return {QPDF_Null::create()};
            }
            addNull();
        }
    }
    return {}; // unreachable
}

void
QPDFParser::add(std::shared_ptr<QPDFObject>&& obj)
{
    frame->olist.emplace_back(std::move(obj));
}

void
QPDFParser::addNull()
{
    const static ObjectPtr null_obj = QPDF_Null::create();

    frame->olist.emplace_back(null_obj);
    ++frame->null_count;
}

template <typename T, typename... Args>
void
QPDFParser::addScalar(Args&&... args)
{
    auto obj = T::create(args...);
    obj->setDescription(context, description, input->getLastOffset());
    add(std::move(obj));
}

template <typename T, typename... Args>
QPDFObjectHandle
QPDFParser::withDescription(Args&&... args)
{
    auto obj = T::create(args...);
    obj->setDescription(context, description, start);
    return {obj};
}

void
QPDFParser::setDescription(ObjectPtr& obj, qpdf_offset_t parsed_offset)
{
    if (obj) {
        obj->setDescription(context, description, parsed_offset);
    }
}

bool
QPDFParser::tooManyBadTokens()
{
    if (good_count <= 4) {
        if (++bad_count > 5) {
            warn("too many errors; giving up on reading object");
            return true;
        }
    } else {
        bad_count = 1;
    }
    good_count = 0;
    return false;
}

void
QPDFParser::warn(QPDFExc const& e) const
{
    // If parsing on behalf of a QPDF object and want to give a warning, we can warn through the
    // object. If parsing for some other reason, such as an explicit creation of an object from a
    // string, then just throw the exception.
    if (context) {
        context->warn(e);
    } else {
        throw e;
    }
}

void
QPDFParser::warn(qpdf_offset_t offset, std::string const& msg) const
{
    warn(QPDFExc(qpdf_e_damaged_pdf, input->getName(), object_description, offset, msg));
}

void
QPDFParser::warn(std::string const& msg) const
{
    warn(input->getLastOffset(), msg);
}
