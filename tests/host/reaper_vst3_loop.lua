-- Real-host check: Reaper renders 12 s with transport running; Aloschen VST3
-- records bar-start impulses on slot 1 (Loop1 envelope flip at 0.2 s), then
-- plays with the editor open (master muted). Driven by tests/host/run_reaper.sh.
local SP = os.getenv("ALO_HOST_DIR") or "."
local logf = io.open(SP .. "/reaper-log.txt", "w")
local function log(s)
  logf:write(s .. "\n")
  logf:flush()
end
reaper.SetCurrentBPM(0, 120, false)
reaper.InsertTrackAtIndex(0, true)
local tr = reaper.GetTrack(0, 0)
reaper.SetOnlyTrackSelected(tr)
reaper.SetEditCurPos(0, false, false)
reaper.InsertMedia(SP .. "/input.wav", 0)
local fx = reaper.TrackFX_AddByName(tr, "VST3:Aloschen", false, -1)
log("fx index " .. tostring(fx))
local n = reaper.TrackFX_GetNumParams(tr, fx)
log("params " .. n)
local idx = {}
for i = 0, n - 1 do
  local _, name = reaper.TrackFX_GetParamName(tr, fx, i, "")
  idx[name] = i
  if i < 50 then
    log(i .. " " .. name)
  end
end
reaper.TrackFX_SetParamNormalized(tr, fx, idx["Click"], 0)
reaper.TrackFX_SetParamNormalized(tr, fx, idx["Bars"], 0) -- 1 bar
local env = reaper.GetFXEnvelope(tr, fx, idx["Loop1"], true)
reaper.InsertEnvelopePoint(env, 0.0, 0, 1, 0, false, true)
reaper.InsertEnvelopePoint(env, 0.2, 1, 1, 0, false, true)
reaper.Envelope_SortPoints(env)
reaper.GetSetProjectInfo(0, "RENDER_BOUNDSFLAG", 0, true)
reaper.GetSetProjectInfo(0, "RENDER_STARTPOS", 0, true)
reaper.GetSetProjectInfo(0, "RENDER_ENDPOS", 12, true)
reaper.GetSetProjectInfo(0, "RENDER_SRATE", 48000, true)
reaper.GetSetProjectInfo(0, "RENDER_CHANNELS", 2, true)
reaper.GetSetProjectInfo_String(0, "RENDER_FILE", SP, true)
reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", "render", true)
reaper.GetSetProjectInfo_String(0, "RENDER_FORMAT", "evaw", true)
reaper.Main_SaveProjectEx(0, SP .. "/aloschen-test.rpp", 0)
reaper.Main_OnCommand(42230, 0)
log("render done")
-- live journey: loop playback with the editor open
reaper.GetSetRepeat(1)
reaper.GetSet_LoopTimeRange(true, true, 0, 12, false)
reaper.SetMediaTrackInfo_Value(reaper.GetMasterTrack(0), "B_MUTE", 1)
reaper.SetEditCurPos(0, false, false)
reaper.TrackFX_Show(tr, fx, 3)
local t0 = reaper.time_precise()
local function go()
  if reaper.time_precise() - t0 < 1.5 then
    reaper.defer(go)
  else
    reaper.OnPlayButton()
    log("playing state " .. reaper.GetPlayState())
  end
end
reaper.defer(go)
