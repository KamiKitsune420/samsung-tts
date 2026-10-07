"""Ask Samsung's store which TTS voice packs it offers.

    python tools/catalogue.py [--json out.json]

Package names are com.samsung.SMT.lang_<code>. The codes and the two device profiles are the ones the NVDA add-on
samsungGalaxyVoices uses (github.com/OnjLouis/samsungGalaxyVoices): the store answers per device model and
Android level, and newer builds of a pack are only offered to newer devices.
"""
import json, re, sys, urllib.parse, urllib.request
from concurrent.futures import ThreadPoolExecutor

CODES = ["en_us_g02", "en_us_l03", "en_us_l04", "en_us_l05", "ko_kr_g01", "ko_kr_l01", "ko_kr_l04", "ko_kr_l08",
         "en_gb_l02", "en_gb_g02", "en_in_l02", "es_es_l01", "es_es_g01", "fr_fr_l01", "fr_fr_g01", "de_de_l01",
         "de_de_g01", "it_it_l01", "it_it_g01", "pt_br_l01", "pt_br_g01", "zh_cn_l02", "zh_cn_g02", "es_us_l01",
         "es_us_g01",
         "cs_cz_f00", "da_dk_f00", "el_gr_f00", "en_au_f00", "en_au_m00", "en_in_f00", "es_mx_f00", "es_mx_m00",
         "es_us_f00", "fi_fi_f00", "fr_ca_f00", "hi_in_f00", "hu_hu_f00", "id_id_f00", "ja_jp_f00", "ja_jp_m00",
         "nb_no_f00", "nl_nl_f00", "pl_pl_f00", "pt_pt_f00", "ro_ro_f00", "ru_ru_f00", "ru_ru_m00", "sk_sk_f00",
         "sv_se_f00", "th_th_f00", "tr_tr_f00", "vi_vn_f00", "zh_cn_f00", "zh_cn_m00", "zh_hk_f00", "zh_tw_f00"]
PROFILES = {"s24": ("SM-S921B", "34"), "legacy": ("SM-G970F", "29")}


def query(code, profile):
    device, sdk = PROFILES[profile]
    params = urllib.parse.urlencode({
        "appId": "com.samsung.SMT.lang_" + code, "deviceId": device, "mcc": "234", "mnc": "15", "csc": "BTU",
        "sdkVer": sdk, "pd": "0", "systemId": "0", "callerId": "com.sec.android.app.samsungapps", "abiType": "64",
        "extuk": "0000000000000000"})
    with urllib.request.urlopen("https://vas.samsungapps.com/stub/stubDownload.as?" + params, timeout=30) as r:
        xml = r.read().decode("utf8", "replace")
    get = lambda tag: (re.search(r"<%s>(?:<!\[CDATA\[)?(.*?)(?:\]\]>)?</%s>" % (tag, tag), xml, re.S) or [None, ""])[1]
    return {"code": code, "profile": profile, "version": get("versionName"), "versionCode": get("versionCode"),
            "size": int(get("contentSize") or 0), "name": get("productName"), "url": get("downloadURI"),
            "message": get("resultMsg")}


def main():
    sys.stdout.reconfigure(encoding="utf8", errors="replace")
    jobs = [(c, p) for c in CODES for p in PROFILES]
    with ThreadPoolExecutor(8) as ex:
        rows = list(ex.map(lambda j: query(*j), jobs))
    ok = [r for r in rows if r["url"]]
    for r in ok:
        print("%-10s %-6s %-12s %6.1f MB  %s" % (r["code"], r["profile"], r["version"], r["size"] / 1e6, r["name"]))
    print("%d of %d (code, profile) pairs offered; %d codes not offered at all: %s" % (
        len(ok), len(rows), len(CODES) - len({r["code"] for r in ok}),
        " ".join(sorted(set(CODES) - {r["code"] for r in ok}))))
    if "--json" in sys.argv:
        with open(sys.argv[sys.argv.index("--json") + 1], "w", encoding="utf8") as f:
            json.dump(ok, f, indent=1)


if __name__ == "__main__":
    main()
