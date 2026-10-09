-- RQUIC's plaintext wire format (not IETF QUIC). No TLS secrets required.
local p = Proto('rquic', 'RQUIC')
local f = {}
local function field(key, kind, label, radix)
    f[key] = ProtoField[kind]('rquic.' .. key, label, radix)
    return f[key]
end
field('flags', 'uint8', 'Header flags', base.HEX)
field('version', 'uint32', 'Version', base.HEX)
field('dcid', 'bytes', 'Destination CID')
field('scid', 'bytes', 'Source CID')
field('length', 'uint64', 'Packet length')
field('packet_number', 'uint32', 'Packet number (truncated)', base.DEC)
field('frame.type', 'uint64', 'Frame type', base.HEX)
for _, key in ipairs({'stream.id', 'stream.offset', 'stream.length', 'ack.largest',
    'ack.delay', 'ack.range_count', 'ack.first_range', 'ack.gap', 'ack.range',
    'ack.ect0', 'ack.ect1', 'ack.ce', 'error_code', 'trigger_frame', 'limit',
    'final_size', 'tp.id', 'tp.length'}) do
    field(key, 'uint64', key, base.DEC)
end
field('stream.fin', 'bool', 'FIN')
field('stream.data', 'bytes', 'Stream data')
field('handshake.alpn', 'string', 'ALPN')
field('handshake.extension', 'bytes', 'Protocol extension')
field('tp.value', 'bytes', 'Transport parameter value')
field('reason', 'string', 'Close reason')
field('path.data', 'bytes', 'Path challenge/response')
field('unknown', 'bytes', 'Unparsed data')
local fields = {}
for _, value in pairs(f) do fields[#fields + 1] = value end
p.fields = fields
local malformed = ProtoExpert.new('rquic.malformed', 'Invalid or truncated RQUIC',
    expert.group.MALFORMED, expert.severity.ERROR)
local unsupported = ProtoExpert.new('rquic.unsupported', 'Unknown frame type',
    expert.group.UNDECODED, expert.severity.WARN)
p.experts = {malformed, unsupported}
p.prefs.ports = Pref.string('UDP ports', '', 'Comma separated ports; empty uses Decode As or heuristic')
p.prefs.cid_length = Pref.uint('Short-header DCID length', 8, 'Match the receiving engine CID length (0..20)')
p.prefs.heuristic = Pref.bool('Detect RQUIC over UDP', true, 'Validate complete packet and frame boundaries before claiming UDP')
local udp = DissectorTable.get('udp.port')
udp:add_for_decode_as(p)
local registered = {}
local names = {[0]='PADDING', [1]='PING', [2]='ACK', [3]='ACK_ECN',
    [4]='RESET_STREAM', [5]='STOP_SENDING', [6]='HANDSHAKE',
    [16]='MAX_DATA', [17]='MAX_STREAM_DATA', [18]='MAX_STREAMS_BIDI',
    [19]='MAX_STREAMS_UNI', [20]='DATA_BLOCKED', [21]='STREAM_DATA_BLOCKED',
    [22]='STREAMS_BLOCKED_BIDI', [23]='STREAMS_BLOCKED_UNI',
    [26]='PATH_CHALLENGE', [27]='PATH_RESPONSE', [28]='CONNECTION_CLOSE', [29]='APPLICATION_CLOSE'}
local function parse(tvb, info, root, dry)
    local pos, ending = 0, tvb:len()
    local summaries = {}
    local function check(n)
        if n < 0 or pos + n > ending then error('Field exceeds packet boundary', 0) end
    end
    local function bytes(n, key, tree)
        check(n)
        local range = tvb(pos, n)
        if tree and key then tree:add(f[key], range) end
        pos = pos + n
        return range
    end
    -- UInt64 arithmetic preserves all 62 bits, unlike Lua floating point.
    local function vint(key, tree)
        check(1)
        local start, first = pos, tvb(pos, 1):uint()
        local n = 2 ^ math.floor(first / 64)
        check(n)
        local value = UInt64(first % 64)
        for i = 1, n - 1 do value = value * UInt64(256) + UInt64(tvb(pos + i, 1):uint()) end
        pos = pos + n
        if tree and key then tree:add(f[key], tvb(start, n), value) end
        return value:tonumber(), value
    end
    local function blob(key, tree)
        local n = vint(nil, nil)
        return bytes(n, key, tree)
    end
    while pos < tvb:len() do
        ending = tvb:len()
        local start = pos
        local flags = bytes(1):uint()
        local long = flags >= 128
        if not long and math.floor(flags / 64) ~= 1 then error('Invalid fixed bit', 0) end
        local tree = root and root:add(p, tvb(start), long and 'RQUIC long header' or 'RQUIC short header')
        if tree then tree:add(f.flags, tvb(start, 1)) end
        local pnlen = flags % 4 + 1
        local version
        if long then
            version = bytes(4, 'version', tree):uint()
            if version ~= 0 and version ~= 1 and version ~= 0xff00001d then error('Unsupported RQUIC version', 0) end
            for _, key in ipairs({'dcid', 'scid'}) do
                local n = bytes(1):uint()
                if n > 20 then error('CID exceeds 20 bytes', 0) end
                bytes(n, key, tree)
            end
        else
            local n = p.prefs.cid_length
            if n > 20 then error('Short-header CID length exceeds 20', 0) end
            bytes(n, 'dcid', tree)
        end
        if long and version == 0 then
            if (ending - pos) < 4 or (ending - pos) % 4 ~= 0 then error('Invalid version negotiation', 0) end
            while pos < ending do bytes(4, 'version', tree) end
            summaries[#summaries + 1] = 'Version negotiation'
        else
            if long and (math.floor(flags / 16) % 4 ~= 0 or flags < 192) then error('Reserved long packet type', 0) end
            local length = vint('length', tree)
            if length < pnlen then error('Length is smaller than packet number', 0) end
            check(length)
            ending = pos + length
            local pn = bytes(pnlen, 'packet_number', tree):uint()
            summaries[#summaries + 1] = 'PN=' .. pn
            while pos < ending do
                local fs = pos
                local ft = tree and tree:add(p, tvb(pos, ending - pos), 'Frame')
                local typ = vint('frame.type', ft)
                local name = names[typ]
                if typ >= 8 and typ <= 15 then name = 'STREAM' end
                if ft then ft:set_text(name or ('Unknown frame ' .. typ)) end
                if not name then
                    if dry then error('Unknown frame', 0) end
                    ft:add_proto_expert_info(unsupported)
                    bytes(ending - pos, 'unknown', ft)
                elseif typ == 0 then
                    while pos < ending and tvb(pos, 1):uint() == 0 do pos = pos + 1 end
                elseif typ == 1 then
                    -- No payload.
                elseif typ == 2 or typ == 3 then
                    vint('ack.largest', ft); vint('ack.delay', ft)
                    local count = vint('ack.range_count', ft)
                    vint('ack.first_range', ft)
                    if count > math.floor((ending - pos) / 2) then error('ACK range count exceeds packet', 0) end
                    for _ = 1, count do vint('ack.gap', ft); vint('ack.range', ft) end
                    if typ == 3 then vint('ack.ect0', ft); vint('ack.ect1', ft); vint('ack.ce', ft) end
                elseif typ == 6 then
                    blob('handshake.alpn', ft)
                    local n = vint(nil, nil)
                    check(n)
                    local outer, tp_end = ending, pos + n
                    ending = tp_end
                    while pos < tp_end do
                        vint('tp.id', ft)
                        local size = vint('tp.length', ft)
                        bytes(size, 'tp.value', ft)
                    end
                    ending = outer
                    blob('handshake.extension', ft)
                elseif typ >= 8 and typ <= 15 then
                    vint('stream.id', ft)
                    if math.floor(typ / 4) % 2 == 1 then vint('stream.offset', ft) end
                    local n = ending - pos
                    if math.floor(typ / 2) % 2 == 1 then n = vint('stream.length', ft) end
                    if ft then ft:add(f['stream.fin'], typ % 2 == 1) end
                    bytes(n, 'stream.data', ft)
                elseif typ == 4 or typ == 5 then
                    vint('stream.id', ft); vint('error_code', ft)
                    if typ == 4 then vint('final_size', ft) end
                elseif typ == 17 or typ == 21 then
                    vint('stream.id', ft); vint('limit', ft)
                elseif typ >= 16 and typ <= 23 then
                    vint('limit', ft)
                elseif typ == 26 or typ == 27 then
                    bytes(8, 'path.data', ft)
                elseif typ == 28 or typ == 29 then
                    vint('error_code', ft)
                    if typ == 28 then vint('trigger_frame', ft) end
                    blob('reason', ft)
                end
                if ft then ft:set_len(pos - fs) end
                if name and typ ~= 0 then summaries[#summaries + 1] = name end
            end
        end
        if tree then tree:set_len(pos - start) end
    end
    if not dry then
        info.cols.info = table.concat(summaries, ', ')
    end
end
function p.dissector(tvb, info, tree)
    info.cols.protocol = 'RQUIC'
    local root = tree:add(p, tvb())
    local ok, err = pcall(parse, tvb, info, root, false)
    if not ok then root:add_proto_expert_info(malformed, tostring(err)) end
    return tvb:len()
end
p:register_heuristic('udp', function(tvb, info, tree)
    if not p.prefs.heuristic or tvb:len() < 7 then return false end
    local ok = pcall(parse, tvb, info, nil, true)
    if not ok then return false end
    p.dissector(tvb, info, tree)
    return true
end)
local function configure()
    for port in pairs(registered) do udp:remove(port, p) end
    registered = {}
    for text in p.prefs.ports:gmatch('[^,%s]+') do
        local port = tonumber(text)
        if port and port >= 1 and port <= 65535 and port == math.floor(port) then
            udp:add(port, p); registered[port] = true
        end
    end
end
p.prefs_changed = configure
configure()
