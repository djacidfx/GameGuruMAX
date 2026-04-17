-- LUA Script - precede every function and global member with lowercase name of script + '_main'
-- Activation Sounder v5
-- DESCRIPTION: Will play or loop a sound when this object is activated.
-- DESCRIPTION: Attach to an object.
-- DESCRIPTION: Set the [@SOUND_STYLE=1(1=Play, 2=Loop)] to play or loop
-- DESCRIPTION: Set the [SOUND_VOLUME=100[1,100] adjust this sounds volume
-- DESCRIPTION: Set the [SOUND_DELAY=0[0,10000] adjust delay before playing sound
-- DESCRIPTION: [SELF_DELETE!=0]
-- DESCRIPTION: <Sound0>

local lower = string.lower

local asobject 			= {}
local sound_style 		= {}
local sound_volume		= {}
local sound_delay		= {}
local self_delete		= {}

local status		= {}
local delay			= {}
local doonce		= {}
local played		= {}
	
function activation_sounder_properties(e, sound_style, sound_volume, sound_delay, self_delete)
	asobject[e].sound_style = sound_style
	asobject[e].sound_volume = sound_volume
	asobject[e].sound_delay = sound_delay
	asobject[e].self_delete = self_delete or 0
end
 
function activation_sounder_init(e)
	asobject[e] = {}
	asobject[e].sound_style = 1
	asobject[e].sound_volume = 100
	asobject[e].sound_delay = 0
	asobject[e].self_delete = 0
	
	status[e] = "init"
	delay[e] = math.huge
	doonce[e] = 0
	played[e] = 0
end
 
function activation_sounder_main(e)
	if status[e] == "init" then
		status[e] = "process"
	end

	if status[e] == "process" then		
		if g_Entity[e]['activated'] == 0 then played[e] = 0	end
		if g_Entity[e]['activated'] == 1 then
			if doonce[e] == 0 then 
				delay[e] = (g_Time + asobject[e].sound_delay)
				doonce[e] = 1
			end	
			if g_Time > delay[e] then
				if played[e] == 0 then
					SetSound(e,0)
					SetSoundVolume(asobject[e].sound_volume)				
					if asobject[e].sound_style == 1	then PlaySound(e,0) end
					if asobject[e].sound_style == 2	then LoopSound(e,0) end				
					played[e] = 1
				end			

				if played[e] == 1 and asobject[e].sound_style == 1 then	
					if asobject[e].self_delete == 1 then
						SwitchScript(e,"no_behavior_selected.lua")
					else
						doonce[e] = 0
						SetActivated(e,0)					
					end
				end
				if played[e] == 1 and asobject[e].sound_style == 2 then
					if g_Entity[e]['health'] <= 0 then
						StopSound(e,0)
						if asobject[e].self_delete == 1 then
							SwitchScript(e,"no_behavior_selected.lua")
						else
							doonce[e] = 0
							SetActivated(e,0)
						end
					end	
				end
			end	
		end
	end
end 
 
function activation_sounder_exit(e) 
end