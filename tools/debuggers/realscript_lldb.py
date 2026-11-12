"""
LLDB Formatter for RealScript Runtime & C++17 AOT Generated Programs.

Usage inside LLDB or VS Code CodeLLDB launch.json (`initCommands`):
    command script import /path/to/tools/debuggers/realscript_lldb.py
"""

import struct


Q16_SCALE = 65536.0

VALUE_TAGS = {
    0: "Monostate",
    1: "NullString",
    2: "NullObject",
    3: "NullArray",
    4: "Bool",
    5: "Byte",
    6: "SByte",
    7: "Short",
    8: "UShort",
    9: "Int64",
    10: "UInt",
    11: "Long",
    12: "ULong",
    13: "Float",
    14: "Double",
    15: "Char",
    16: "EnumInline",
    17: "Fixed",
    18: "Fixed16",
    19: "Vec2",
    20: "Vec3",
    21: "QuatInline",
    22: "NativeHandle",
    23: "EnumArena",
    24: "QuatArena",
    25: "Struct",
    26: "String",
    27: "ObjectRef",
}


def _read_bytes(valobj, size=16):
    err = valobj.GetProcess().GetSelectedThread()  # ensure alive
    _ = err
    import lldb
    error = lldb.SBError()
    addr = valobj.GetLoadAddress()
    if addr != lldb.LLDB_INVALID_ADDRESS:
        data = valobj.GetProcess().ReadMemory(addr, size, error)
        if error.Success() and data and len(data) == size:
            return data
    bdata = valobj.GetData()
    if bdata.GetByteSize() >= size:
        raw = [bdata.GetUnsignedInt8(error, i) for i in range(size)]
        if error.Success():
            return bytes(raw)
    return None


def format_value_bytes(raw, valobj=None):
    if not raw or len(raw) < 16:
        return "<unavailable>"
    vtag = raw[15]
    if vtag == 0:
        return "[uninitialized]"
    if vtag == 1:
        return "null (string)"
    if vtag == 2:
        return "null (object)"
    if vtag == 3:
        return "null (array)"
    if vtag == 4:
        return "true" if raw[0] != 0 else "false"
    if vtag == 5:
        return f"(byte) {raw[0]}"
    if vtag == 6:
        return f"(sbyte) {struct.unpack_from('<b', raw, 0)[0]}"
    if vtag == 7:
        return f"(short) {struct.unpack_from('<h', raw, 0)[0]}"
    if vtag == 8:
        return f"(ushort) {struct.unpack_from('<H', raw, 0)[0]}"
    if vtag == 9:
        return f"{struct.unpack_from('<q', raw, 0)[0]}"
    if vtag == 10:
        return f"{struct.unpack_from('<I', raw, 0)[0]}u"
    if vtag == 11:
        return f"{struct.unpack_from('<q', raw, 0)[0]}L"
    if vtag == 12:
        return f"{struct.unpack_from('<Q', raw, 0)[0]}UL"
    if vtag == 13:
        return f"{struct.unpack_from('<f', raw, 0)[0]:.6g}f"
    if vtag == 14:
        return f"{struct.unpack_from('<d', raw, 0)[0]:.12g}"
    if vtag == 15:
        return f"(char) {struct.unpack_from('<H', raw, 0)[0]}"
    if vtag == 16:
        tid, v32 = struct.unpack_from("<Qi", raw, 0)
        return f"Enum(0x{tid:x}) = {v32}"
    if vtag == 17:
        r32 = struct.unpack_from("<i", raw, 0)[0]
        return f"{r32 / Q16_SCALE:.4f} (raw={r32})"
    if vtag == 18:
        r64 = struct.unpack_from("<q", raw, 0)[0]
        return f"{r64 / Q16_SCALE:.4f}"
    if vtag == 19:
        x, y = struct.unpack_from("<ii", raw, 0)
        return f"Vec2({x / Q16_SCALE:.4f}, {y / Q16_SCALE:.4f})"
    if vtag == 20:
        x, y, z = struct.unpack_from("<iii", raw, 0)
        return f"Vec3({x / Q16_SCALE:.4f}, {y / Q16_SCALE:.4f}, {z / Q16_SCALE:.4f})"
    if vtag == 21:
        x, y, z = struct.unpack_from("<iii", raw, 0)
        w_u24 = raw[12] | (raw[13] << 8) | (raw[14] << 16)
        w = w_u24 - 0x1000000 if (w_u24 & 0x800000) else w_u24
        return (
            f"Quat({x / Q16_SCALE:.4f}, {y / Q16_SCALE:.4f}, "
            f"{z / Q16_SCALE:.4f}, {w / Q16_SCALE:.4f})"
        )
    if vtag == 22:
        slot, gen, reg_id, type_idx = struct.unpack_from("<IHHI", raw, 0)
        if slot == 0xFFFFFFFF or gen == 0 or reg_id == 0 or type_idx == 0:
            return "null NativeHandle"
        return f"NativeHandle(slot={slot}, gen={gen}, reg={reg_id}, typeIdx={type_idx})"
    if vtag == 23:
        arena_idx = struct.unpack_from("<I", raw, 0)[0]
        return f"EnumArena(idx={arena_idx})"
    if vtag == 24:
        arena_idx = struct.unpack_from("<I", raw, 0)[0]
        return f"QuatArena(idx={arena_idx})"
    if vtag == 25:
        tid = struct.unpack_from("<Q", raw, 0)[0]
        return f"Struct(typeId=0x{tid:x})"
    if vtag == 26:
        ptr_addr, intern_id = struct.unpack_from("<QI", raw, 0)
        if valobj is not None:
            u_val = valobj.GetChildMemberWithName("u_")
            s_val = u_val.GetChildMemberWithName("stringVal") if u_val and u_val.IsValid() else None
            p_val = s_val.GetChildMemberWithName("ptr") if s_val and s_val.IsValid() else None
            if p_val and p_val.IsValid() and p_val.GetValueAsUnsigned() != 0:
                summary = p_val.Dereference().GetSummary()
                if summary:
                    return summary
        if ptr_addr == 0 and intern_id == 0:
            return '""'
        return f"<string internId={intern_id}>"
    if vtag == 27:
        slot, gen, hid, kind = struct.unpack_from("<IIIB", raw, 0)
        if slot == 0xFFFFFFFF or gen == 0 or hid == 0:
            return "null ObjectRef"
        kname = {0: "String", 1: "Array", 2: "Record"}.get(kind, "Object")
        return f"{kname}(slot={slot}, gen={gen}, heap={hid})"
    return f"Value({VALUE_TAGS.get(vtag, str(vtag))})"


def value_summary(valobj, _internal_dict):
    raw = _read_bytes(valobj, 16)
    return format_value_bytes(raw, valobj)


def string_summary(valobj, _internal_dict):
    p_val = valobj.GetChildMemberWithName("ptr")
    id_val = valobj.GetChildMemberWithName("id")
    if p_val and p_val.IsValid() and p_val.GetValueAsUnsigned() != 0:
        summary = p_val.Dereference().GetSummary()
        if summary:
            return summary
    intern_id = id_val.GetValueAsUnsigned() if id_val and id_val.IsValid() else 0
    return '""' if intern_id == 0 else f'"" (internId={intern_id})'


def _signed_field(valobj, name):
    child = valobj.GetChildMemberWithName(name)
    return child.GetValueAsSigned() if child and child.IsValid() else 0


def fixed_summary(valobj, _internal_dict):
    raw = _signed_field(valobj, "raw")
    return f"{raw / Q16_SCALE:.4f} (raw={raw})"


def vec2_summary(valobj, _internal_dict):
    x = _signed_field(valobj, "x") / Q16_SCALE
    y = _signed_field(valobj, "y") / Q16_SCALE
    return f"({x:.4f}, {y:.4f})"


def vec3_summary(valobj, _internal_dict):
    x = _signed_field(valobj, "x") / Q16_SCALE
    y = _signed_field(valobj, "y") / Q16_SCALE
    z = _signed_field(valobj, "z") / Q16_SCALE
    return f"({x:.4f}, {y:.4f}, {z:.4f})"


def quat_summary(valobj, _internal_dict):
    x = _signed_field(valobj, "x") / Q16_SCALE
    y = _signed_field(valobj, "y") / Q16_SCALE
    z = _signed_field(valobj, "z") / Q16_SCALE
    w = _signed_field(valobj, "w") / Q16_SCALE
    return f"({x:.4f}, {y:.4f}, {z:.4f}, {w:.4f})"


def __lldb_init_module(debugger, _internal_dict):
    debugger.HandleCommand(
        'type summary add -F realscript_lldb.value_summary "realscript::runtime::Value"'
    )
    debugger.HandleCommand(
        'type summary add -F realscript_lldb.string_summary "realscript::runtime::StringValue"'
    )
    debugger.HandleCommand(
        'type summary add -F realscript_lldb.fixed_summary "realscript::runtime::FixedValue"'
    )
    debugger.HandleCommand(
        'type summary add -F realscript_lldb.fixed_summary "realscript::runtime::Fixed16Value"'
    )
    debugger.HandleCommand(
        'type summary add -F realscript_lldb.vec2_summary "realscript::runtime::Vec2Value"'
    )
    debugger.HandleCommand(
        'type summary add -F realscript_lldb.vec3_summary "realscript::runtime::Vec3Value"'
    )
    debugger.HandleCommand(
        'type summary add -F realscript_lldb.quat_summary "realscript::runtime::QuatValue"'
    )
