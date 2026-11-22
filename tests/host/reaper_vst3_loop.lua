-- Render the installed VST3 in REAPER, then expose its editor during muted playback.
local dir = assert(os.getenv("ALO_HOST_DIR"), "run tests/host/run_reaper.sh")
local mode = os.getenv("ALO_HOST_MODE") or "loop"
local log_file = assert(io.open(dir .. "/reaper-log.txt", "w"))
local function log(s)
  log_file:write(s .. "\n")
  log_file:flush()
end

local bpm, num, denom = 120, 4, 4
if mode == "click-7-8" then
  bpm, num, denom = 131, 7, 8
end
reaper.SetTempoTimeSigMarker(0, -1, 0, -1, -1, bpm, num, denom, false)
if mode == "click-tempo" then
  -- Four beats at 120 BPM, then two bars at 96 BPM.
  reaper.SetTempoTimeSigMarker(0, -1, 2, -1, -1, 96, 4, 4, false)
end
reaper.GetSetProjectInfo(0, "PROJECT_SRATE", 48000, true)
reaper.GetSetProjectInfo(0, "PROJECT_SRATE_USE", 1, true)
reaper.InsertTrackAtIndex(0, true)
local tr = reaper.GetTrack(0, 0)
reaper.SetOnlyTrackSelected(tr)
if mode == "loop" then
  reaper.SetEditCurPos(0, false, false)
  reaper.InsertMedia(dir .. "/input.wav", 0)
end

local fx = reaper.TrackFX_AddByName(tr, "VST3:Aloschen", false, -1)
if fx < 0 then
  log("ERROR: VST3:Aloschen missing; install the built bundle in ~/.vst3")
  return
end
local idx = {}
for i = 0, reaper.TrackFX_GetNumParams(tr, fx) - 1 do
  local _, name = reaper.TrackFX_GetParamName(tr, fx, i, "")
  idx[name] = i
end
reaper.TrackFX_SetParamNormalized(tr, fx, assert(idx["Click"]), mode == "loop" and 0 or 1)
reaper.TrackFX_SetParamNormalized(tr, fx, assert(idx["Bars"]), 0)
if mode == "loop" then
  reaper.TrackFX_SetParamNormalized(tr, fx, assert(idx["Mix"]), 1)
  local env = reaper.GetFXEnvelope(tr, fx, assert(idx["Loop1"]), true)
  reaper.InsertEnvelopePoint(env, 0.0, 0, 1, 0, false, true)
  reaper.InsertEnvelopePoint(env, 0.2, 1, 1, 0, false, true)
  reaper.Envelope_SortPoints(env)
end
reaper.GetSetProjectInfo(0, "RENDER_BOUNDSFLAG", 0, true)
reaper.GetSetProjectInfo(0, "RENDER_STARTPOS", 0, true)
reaper.GetSetProjectInfo(0, "RENDER_ENDPOS", 12, true)
reaper.GetSetProjectInfo(0, "RENDER_SRATE", 48000, true)
reaper.GetSetProjectInfo(0, "RENDER_CHANNELS", 2, true)
reaper.GetSetProjectInfo_String(0, "RENDER_FILE", dir, true)
reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", "render", true)
reaper.GetSetProjectInfo_String(0, "RENDER_FORMAT", "evaw", true)
reaper.Main_SaveProjectEx(0, dir .. "/aloschen-test.rpp", 0)
reaper.Main_OnCommand(42230, 0)
log("render done: " .. mode)

-- Keep the live run silent at speakers while the plugin's own meters are active.
reaper.GetSetRepeat(1)
reaper.GetSet_LoopTimeRange(true, true, 0, 12, false)
reaper.SetMediaTrackInfo_Value(reaper.GetMasterTrack(0), "B_MUTE", 1)
reaper.SetEditCurPos(0, false, false)
reaper.TrackFX_Show(tr, fx, 3)
reaper.OnPlayButton()
log("playing state " .. reaper.GetPlayState())
