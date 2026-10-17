"""More German "Pixel" clips (round 2): OpenAI voices with new styles, the clean Piper voices."""
import concurrent.futures as cf, random
import gen_dataset as g
rng = random.Random(77)
key = g.openai_key()
styles2 = ["Sprich mit deutschem Akzent, ganz normal, als wuerdest du deinen Roboter rufen.",
           "Sag es nebenbei, waehrend du etwas anderes machst.", "Sprich laut und deutlich, etwas ungeduldig.",
           "Sag es zaertlich und leise.", "Sprich wie ein Jugendlicher, locker.", "Ruf es froehlich, langgezogen: Piiixel.",
           "Sag es wie eine aeltere Person, ruhig.", "Sprich schnell, wie ein kurzer Ruf.", "Sag es ueberrascht.",
           "Sprich mit vollem Mund, undeutlich.", "Sprich im bayerischen Dialekt.", "Sprich im saechsischen Dialekt.",
           "Sag es verschlafen am Morgen.", "Sprich wie ein Nachrichtensprecher.", "Sag es frech und verspielt."]
texts = ["Pixel", "Pixel!", "Pixel?", "Hey Pixel", "Hallo Pixel", "Pixel.", "Ey Pixel", "Pixel, du"]
jobs = []
for v in g.OAI_VOICES:
    for si, st in enumerate(styles2):
        jobs.append((rng.choice(texts), v, st, g.DATA / "pos" / f"oai2_{v}_{si:02d}.wav"))
with cf.ThreadPoolExecutor(8) as ex:
    list(ex.map(lambda j: g.oai_tts(key, *j[:3], j[3]), jobs))
for lang, loc, name, q in g.PIPER:
    if name in ("mls", "libritts_r"): continue
    v = g.piper_voice(lang, loc, name, q)
    n = v.config.num_speakers
    for k in range(120 if n == 1 else 25):
        for s in ([None] if n == 1 else range(n)):
            sid = "" if s is None else f"{s}_"
            g.piper_say(v, rng.choice(texts), g.DATA / "pos" / f"pp2_{name}_{q}_{sid}{k}.wav", s)
print("done")
