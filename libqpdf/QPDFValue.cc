#include <qpdf/QPDFValue.hh>

#include <qpdf/QPDFObject_private.hh>

std::shared_ptr<QPDFObject>
QPDFValue::do_create(QPDFValue* object)
{
    std::shared_ptr<QPDFObject> obj(new QPDFObject());
    obj->value = std::shared_ptr<QPDFValue>(object);
    return obj;
}

std::string
QPDFValue::getDescription()
{
    if (object_description) {
        switch (object_description->index()) {
        case 0:
            {
                auto description = std::get<0>(*object_description);

                if (auto pos = description.find("$OG");
                    pos != std::string::npos) {
                    description.replace(pos, 3, og.unparse(' '));
                }
                if (auto pos = description.find("$PO");
                    pos != std::string::npos) {
                    qpdf_offset_t shift = (type_code == ::ot_dictionary) ? 2
                        : (type_code == ::ot_array)                      ? 1
                                                                         : 0;

                    description.replace(
                        pos, 3, std::to_string(parsed_offset + shift));
                }
                return description;
            }
        case 1:
            {
                auto j_descr = std::get<1>(*object_description);
                return (
                    *j_descr.input +
                    (j_descr.object.empty() ? "" : ", " + j_descr.object) +
                    " at offset " + std::to_string(parsed_offset));
            }
        }
    } else if (og.isIndirect()) {
        return "object " + og.unparse(' ');
    }
    return {};
}
