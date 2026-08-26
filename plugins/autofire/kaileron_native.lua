-- license:BSD-3-Clause
-- Kaileron native autofire configuration UI.  This module deliberately owns
-- no frame counter and never calls ioport_field:set_value.  The adapter samples
-- the saved logical-button policy once, before rollback input is cached.

local exports = {}

local allowed = os.getenv('KN_AUTOFIRE_ALLOWED') == '1'
local config_path = os.getenv('KN_AUTOFIRE_CONFIG') or ''
local interval = 2
local enabled = {}
local button_fields = {}
local stop_subscription

local function load_settings()
	interval = 2
	enabled = {}
	local file = io.open(config_path, 'r')
	if file then
		for line in file:lines() do
			local value = line:match('^interval=(%d+)$')
			if value then
				interval = math.max(2, math.min(10, tonumber(value)))
			else
				value = line:match('^button=(%d+)$')
				if value then
					value = tonumber(value)
					if value >= 1 and value <= 16 then enabled[value] = true end
				end
			end
		end
		file:close()
	end

	button_fields = {}
	local ioport = manager.machine.ioport
	for _, port in pairs(ioport.ports) do
		for _, field in pairs(port.fields) do
			local token = ioport:input_type_to_token(field.type, field.player)
			local button = token and token:match('BUTTON(%d+)$')
			button = button and tonumber(button) or nil
			if field.player == 0 and button and button >= 1 and button <= 16 and not button_fields[button] then
				button_fields[button] = field
			end
		end
	end
end

local function save_settings()
	if config_path == '' then return end
	local directory = config_path:match('^(.*)[/\\][^/\\]+$')
	if directory and not lfs.attributes(directory) then lfs.mkdir(directory) end
	local file = io.open(config_path, 'w')
	if not file then
		emu.print_error('Kaileron autofire: unable to write ' .. config_path)
		return
	end
	file:write('version=1\ninterval=', tostring(interval), '\n')
	for button = 1, 16 do
		if enabled[button] then file:write('button=', tostring(button), '\n') end
	end
	file:close()
end

local function binding_name(field)
	local sequence = field:input_seq('standard')
	return manager.machine.input:seq_name(sequence)
end

local function populate_menu()
	local menu = {
		{'Kaileron Autofire / 연사', '', 'off'},
		{'Room policy / 방 정책', allowed and 'Allowed / 허용' or 'Disabled / 금지', 'off'},
		{'Interval / 연사 간격', tostring(interval), allowed and 'lr' or 'off'},
		{'---', '', ''}
	}
	local count = 0
	for button = 1, 16 do
		local field = button_fields[button]
		if field then
			count = count + 1
			local label = string.format('%s (%s)', field.name, binding_name(field))
			local value = enabled[button] and 'Autofire / 연사' or 'Normal / 일반'
			table.insert(menu, {label, value, allowed and 'lr' or 'off'})
		end
	end
	if count == 0 then
		table.insert(menu, {'No digital buttons / 디지털 버튼 없음', '', 'off'})
	end
	return menu
end

local function menu_callback(index, event)
	manager.machine:popmessage()
	if not allowed then
		if event == 'select' then manager.machine:popmessage('Autofire is disabled by the room host. / 방장이 연사를 금지했습니다.') end
		return false
	end
	if index == 3 then
		if event == 'left' then
			interval = math.max(2, interval - 1)
		elseif event == 'right' then
			interval = math.min(10, interval + 1)
		elseif event == 'clear' then
			interval = 2
		else
			return false
		end
		save_settings()
		return true
	end

	local visible = 4
	for button = 1, 16 do
		if button_fields[button] then
			visible = visible + 1
			if index == visible and (event == 'select' or event == 'left' or event == 'right' or event == 'clear') then
				enabled[button] = not enabled[button]
				save_settings()
				return true
			end
		end
	end
	return false
end

function exports.startplugin()
	emu.register_prestart(load_settings)
	stop_subscription = emu.add_machine_stop_notifier(save_settings)
	emu.register_menu(menu_callback, populate_menu, 'Kaileron Autofire / 연사')
end

return exports
