#!/usr/bin/lua

local path = assert(arg[1], "missing JavaScript path")
local file = assert(io.open(path, "rb"))
local source = file:read("*a")
file:close()

if source:find('screenplus:"ScreenPlus"', 1, true) then
	os.exit(0)
end

local replacements = {
	{
		'guest_wifi:this.$t("btnsettings.guest_wifi")}}',
		'guest_wifi:this.$t("btnsettings.guest_wifi"),screenplus:"ScreenPlus"}}'
	},
	{
		'adguardhome:6,led:7}',
		'adguardhome:6,led:7,screenplus:8}'
	},
	{
		'checkFuncStatus(){(function(t){return s("call",["sid","switch-button","check_sync_status",t])})(this.funcParams).then((t=>{t&&t.err_msg||(0===t.sync_status?this.showTips=!0:this.setBtnConfig())}))}',
		'checkFuncStatus(){this.func.includes("screenplus")?this.setBtnConfig(!0):(function(t){return s("call",["sid","switch-button","check_sync_status",t])})(this.funcParams).then((t=>{t&&t.err_msg||(0===t.sync_status?this.showTips=!0:this.setBtnConfig())}))}'
	}
}

for _, replacement in ipairs(replacements) do
	local first, last = source:find(replacement[1], 1, true)
	assert(first, "unsupported GL Toggle frontend")
	assert(not source:find(replacement[1], last + 1, true), "ambiguous GL Toggle frontend")
	source = source:sub(1, first - 1) .. replacement[2] .. source:sub(last + 1)
end

local output = assert(io.open(path, "wb"))
assert(output:write(source))
assert(output:close())
