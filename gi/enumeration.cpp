/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil; -*- */
// SPDX-License-Identifier: MIT OR LGPL-2.0-or-later
// SPDX-FileCopyrightText: 2008 litl, LLC

#include <config.h>

#include <inttypes.h>

#include <girepository.h>
#include <glib-object.h>
#include <glib.h>

#include <js/PropertyAndElement.h>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>
#include <jsapi.h>  // for JS_NewPlainObject

#include "gi/cwrapper.h"
#include "gi/enumeration.h"
#include "gi/info.h"
#include "gi/wrapperutils.h"
#include "gjs/auto.h"
#include "gjs/jsapi-util.h"
#include "gjs/macros.h"
#include "util/log.h"

GJS_JSAPI_RETURN_CONVENTION
static bool
gjs_define_enum_value(JSContext       *context,
                      JS::HandleObject in_object,
                      GIValueInfo     *info)
{
    const char* value_name;
    gsize i;
    gint64 value_val;

    value_name = g_base_info_get_name( (GIBaseInfo*) info);
    value_val = g_value_info_get_value(info);

    /* g-i converts enum members such as GDK_GRAVITY_SOUTH_WEST to
     * Gdk.GravityType.south-west (where 'south-west' is value_name)
     * Convert back to all SOUTH_WEST.
     */
    Gjs::AutoChar fixed_name{g_ascii_strup(value_name, -1)};
    for (i = 0; fixed_name[i]; ++i) {
        char c = fixed_name[i];
        if (!(('A' <= c && c <= 'Z') ||
              ('0' <= c && c <= '9')))
            fixed_name[i] = '_';
    }

    gjs_debug(GJS_DEBUG_GENUM,
              "Defining enum value %s (fixed from %s) %" PRId64,
              fixed_name.get(), value_name, value_val);

    if (!JS_DefineProperty(context, in_object,
                           fixed_name, (double) value_val,
                           GJS_MODULE_PROP_FLAGS)) {
        gjs_throw(context,
                  "Unable to define enumeration value %s %" G_GINT64_FORMAT
                  " (no memory most likely)",
                  fixed_name.get(), value_val);
        return false;
    }

    return true;
}

bool
gjs_define_enum_values(JSContext       *context,
                       JS::HandleObject in_object,
                       GIEnumInfo      *info)
{
    int i, n_values;

    /* Fill in enum values first, so we don't define the enum itself until we're
     * sure we can finish successfully.
     */
    n_values = g_enum_info_get_n_values(info);
    for (i = 0; i < n_values; ++i) {
        GI::AutoBaseInfo value_info{g_enum_info_get_value(info, i)};

        if (!gjs_define_enum_value(context, in_object, value_info))
            return false;
    }
    return true;
}

bool
gjs_define_enumeration(JSContext       *context,
                       JS::HandleObject in_object,
                       GIEnumInfo      *info)
{
    const char *enum_name;

    /* An enumeration is simply an object containing integer attributes for
     * each enum value. It does not have a special JSClass.
     *
     * We could make this more typesafe and also print enum values as strings
     * if we created a class for each enum and made the enum values instances
     * of that class. However, it would have a lot more overhead and just
     * be more complicated in general. I think this is fine.
     */

    enum_name = g_base_info_get_name( (GIBaseInfo*) info);

    JS::RootedObject enum_obj(context, JS_NewPlainObject(context));
    if (!enum_obj) {
        gjs_throw(context, "Could not create enumeration %s.%s",
                  g_base_info_get_namespace(info), enum_name);
        return false;
    }

    GType gtype = g_registered_type_info_get_g_type(info);

    if (!gjs_define_enum_values(context, enum_obj, info) ||
        !gjs_define_static_methods<InfoType::Enum>(context, enum_obj, gtype,
                                                   info) ||
        !gjs_wrapper_define_gtype_prop(context, enum_obj, gtype))
        return false;

    gjs_debug(GJS_DEBUG_GENUM,
              "Defining %s.%s as %p",
              g_base_info_get_namespace( (GIBaseInfo*) info),
              enum_name, enum_obj.get());

    if (!JS_DefineProperty(context, in_object, enum_name, enum_obj,
                           GJS_MODULE_PROP_FLAGS)) {
        gjs_throw(context, "Unable to define enumeration property (no memory most likely)");
        return false;
    }

    return true;
}
