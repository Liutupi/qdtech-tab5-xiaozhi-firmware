"""NABO FM SD catalog: script filters, binary format, and the device reader."""
import importlib.util
import shutil
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/qdtech/tab5"
TESTS = Path(__file__).parent
SCRIPT = ROOT / "scripts/prepare_nabo_radio_catalog.py"


def load_script():
    spec = importlib.util.spec_from_file_location("prepare_nabo_radio_catalog", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def station(name, url, region="广东", types=("新闻",), codec="mp3", health="ok", heat=10, py=""):
    return dict(name=name, url=url, codec=codec, region=region, types=list(types), heat=heat,
                health=health, py=py)


def qt(n):
    return f"https://lhttp.qtfm.cn/live/{n}/64k.mp3"


SAMPLE = [
    station("广东新闻广播", qt(1254), py="gdxwgb", heat=90000),
    station("河北交通广播", qt(1648), region="河北", types=("交通",), py="hbjtgb"),
    station("香港电台第一台", "https://stm1.rthk.hk/radio1", region="香港", types=("综合",), py="xgdt"),
    station("Jazz FM", "https://example.com/jazz.mp3", region="网络台", types=("音乐", "文艺"), py="jf"),
    station("AAC 台", "https://example.com/a.aac", codec="aac"),
    station("伪装 AAC", "https://example.com/b.aac"),
    station("跨域台", "https://example.com/c.mp3", health="cors"),
    station("重复台", qt(1254)),
    station("", "https://example.com/noname.mp3"),
]


class CatalogScriptTests(unittest.TestCase):
    def test_build_filters_and_round_trips(self):
        tool = load_script()
        data, report = tool.build(SAMPLE)
        self.assertEqual(report["stations"], 4)
        self.assertEqual(report["skipped"], {"codec": 1, "url": 2, "cors": 1, "duplicate": 1})
        parsed = tool.read(data)
        names = [s["name"] for s in parsed["stations"]]
        self.assertEqual(names, ["广东新闻广播", "河北交通广播", "香港电台第一台", "Jazz FM"])
        first, jazz = parsed["stations"][0], parsed["stations"][3]
        self.assertEqual((first["id"], first["region"], first["types"], first["heat"]),
                         (1254, "广东", ["新闻"], 65535))
        self.assertEqual(sorted(jazz["types"]), ["文艺", "音乐"])
        self.assertTrue(jazz["id"] & 0x80000000)
        self.assertEqual(jazz["id"], 0x80000000 | (zlib.crc32(b"https://example.com/jazz.mp3") & 0x7FFFFFFF))

    def test_build_rejects_oversized_names_and_id_collisions(self):
        tool = load_script()
        with self.assertRaises(ValueError):
            tool.build([station("长" * 40, qt(1))])
        with self.assertRaises(ValueError):
            tool.build([station("甲", qt(7)), station("乙", "https://lhttp.qingting.fm/live/7/64k.mp3")])

    def test_read_rejects_corruption(self):
        tool = load_script()
        data, _ = tool.build(SAMPLE)
        corrupt = bytearray(data)
        corrupt[tool.HEADER.size + 12] ^= 1
        with self.assertRaises(ValueError):
            tool.read(bytes(corrupt))
        with self.assertRaises(ValueError):
            tool.read(data[:-1])


@unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
class CatalogDeviceReaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.tmp.name) / "smoke"
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(BOARD),
                        str(TESTS / "nabo_radio_catalog_smoke.cc"), "-o", str(cls.binary)], check=True)
        cls.tool = load_script()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_smoke(self, *args, check=True):
        return subprocess.run([str(self.binary), *map(str, args)], check=check, capture_output=True,
                              text=True).stdout

    def write(self, name, data):
        path = Path(self.tmp.name) / name
        path.write_bytes(data)
        return path

    def test_device_reader_matches_script(self):
        # Enough stations that the string pool spans many Find windows.
        stations = SAMPLE + [station(f"测试电台{i}", qt(5000 + i), region=f"省{i % 40}",
                                     types=(("新闻", "交通", "音乐", "文艺", "生活")[i % 5],),
                                     py=f"csdt{i}", heat=i) for i in range(1500)]
        data, report = self.tool.build(stations)
        parsed = self.tool.read(data)
        out = self.run_smoke("dump", self.write("big.bin", data)).splitlines()
        self.assertEqual(out[0], f"resident {report['resident_bytes']}")
        lines = {}
        for line in out[1:]:
            kind, index, rest = line.split(" ", 2)
            lines[(kind, int(index))] = rest
        for i, s in enumerate(parsed["stations"]):
            mask = sum(1 << parsed["types"].index(t) for t in s["types"])
            self.assertEqual(lines[("station", i)],
                             f"{s['id']} {s['heat']} {mask} {s['region']}|{s['name']}|{s['url']}|{s['py']}")
        for i, region in enumerate(parsed["regions"]):
            self.assertTrue(lines[("region", i)].startswith(region + " "))
        self.assertEqual(lines[("region", parsed["regions"].index("香港"))], "香港 3")
        self.assertEqual(lines[("region", parsed["regions"].index("网络台"))], "网络台 2")
        self.assertEqual(lines[("region", parsed["regions"].index("广东"))], "广东 0")

        path = Path(self.tmp.name) / "big.bin"
        index = {s["name"]: i for i, s in enumerate(parsed["stations"])}
        self.assertEqual(self.run_smoke("find", path, "测试电台1499").strip(), str(index["测试电台1499"]))
        # Contains beats pinyin; the hotter (earlier) station wins ties.
        self.assertEqual(self.run_smoke("find", path, "交通").strip(), str(index["河北交通广播"]))
        self.assertEqual(self.run_smoke("find", path, "JAZZ").strip(), str(index["Jazz FM"]))
        self.assertEqual(self.run_smoke("find", path, "csdt777").strip(), str(index["测试电台777"]))
        self.assertEqual(self.run_smoke("find", path, "hbjt").strip(), str(index["河北交通广播"]))
        self.assertEqual(self.run_smoke("find", path, "不存在").strip(), "-1")

    def test_device_reader_rejects_corruption(self):
        data, _ = self.tool.build(SAMPLE)
        for name, bad in [("crc.bin", data[:60] + bytes([data[60] ^ 1]) + data[61:]),
                          ("short.bin", data[:-1]),
                          ("magic.bin", b"XFMC" + data[4:])]:
            result = subprocess.run([str(self.binary), "dump", str(self.write(name, bad))],
                                    capture_output=True, text=True)
            self.assertEqual(result.stdout.strip(), "rejected", name)

    def test_stable_ids_match(self):
        for url in [qt(15318317), "https://lhttp-hw.qtfm.cn/live/339/64k.mp3",
                    "http://lhttp.qingting.fm/live/4938/64k.mp3", "https://ice1.somafm.com/groovesalad-128-mp3",
                    "https://lhttp.qtfm.cn/live/abc/64k.mp3"]:
            self.assertEqual(int(self.run_smoke("id", url)), self.tool.stable_id(url), url)
        self.assertEqual(self.tool.stable_id(qt(339)), 339)
        self.assertEqual(self.tool.stable_id("https://lhttp-hw.qtfm.cn/live/339/64k.mp3"), 339)


@unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
class RadioCatalogBandTests(unittest.TestCase):
    """radio_catalog.cc on the host: dial bands, labels and the fallback list."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.tmp.name) / "bands"
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-I", str(TESTS / "radio_catalog_stubs"), "-I", str(BOARD),
                        str(TESTS / "radio_catalog_smoke.cc"), str(BOARD / "radio_catalog.cc"),
                        "-o", str(cls.binary)], check=True)
        stations = SAMPLE + [
            station("广东音乐之声", qt(1271), types=("音乐",), py="gdyyzs"),
            station("河北文艺广播", qt(1650), region="河北", types=("文艺",), py="hbwygb"),
            station("网络综合台", "https://example.com/net.mp3", region="网络台", types=("综合",)),
            station("山西综合广播", qt(1700), region="山西", types=("综合",)),
            station("河北生活广播", qt(1651), region="河北", types=("生活",)),
        ]
        data, _ = load_script().build(stations)
        cls.catalog = Path(cls.tmp.name) / "catalog.bin"
        cls.catalog.write_bytes(data)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_bands(self, *args):
        out = subprocess.run([str(self.binary), *map(str, args)], check=True, capture_output=True,
                             text=True).stdout.splitlines()
        stations = {}
        bands = {}
        finds = {}
        for line in out[1:]:
            kind, rest = line.split(" ", 1)
            if kind == "station":
                index, fields = rest.split(" ", 1)
                stations[int(index)] = fields.split("|")
            elif kind == "band":
                name, *members = rest.split(" ")
                bands[name] = [int(m) for m in members]
            elif kind == "find":
                needle, index, *name = rest.split(" ")
                finds[needle] = (int(index), " ".join(name))
        return out[0], stations, bands, finds

    def test_card_bands_follow_region_and_type(self):
        source, stations, bands, finds = self.run_bands(self.catalog, "河北", 1650, 999, 1254)
        self.assertEqual(source, "source card 9")
        index = {fields[3]: i for i, fields in stations.items()}
        self.assertEqual(list(bands), ["最近", "本地", "新闻", "音乐", "交通", "文艺", "各省", "港澳·海外"])
        self.assertEqual(bands["最近"], [index["河北文艺广播"], index["广东新闻广播"]])
        self.assertEqual(bands["本地"], [index[n] for n in ["河北交通广播", "河北文艺广播", "河北生活广播"]])
        self.assertEqual(bands["音乐"], [index["Jazz FM"], index["广东音乐之声"]])
        self.assertEqual(bands["文艺"], [index["Jazz FM"], index["河北文艺广播"]])
        self.assertEqual(bands["港澳·海外"], [index["香港电台第一台"]])
        # Province by province, the bigger first; never 网络台 or 香港.
        self.assertEqual(bands["各省"], [index[n] for n in ["河北交通广播", "河北文艺广播", "河北生活广播",
                                                           "广东新闻广播", "广东音乐之声", "山西综合广播"]])
        hk = stations[index["香港电台第一台"]]
        self.assertEqual(hk[1:3], ["港澳·海外", "香港 · 综合"])
        self.assertEqual(stations[index["Jazz FM"]][1], "音乐")
        self.assertEqual(stations[index["山西综合广播"]][1], "各省")
        self.assertEqual(stations[index["网络综合台"]][1], "")
        gd = stations[index["广东新闻广播"]]
        self.assertEqual(gd[:3], ["1254", "新闻", "广东 · 新闻"])
        self.assertEqual(gd[4:], [qt(1254), "https://lhttp-hw.qtfm.cn/live/1254/64k.mp3",
                                  "http://lhttp.qingting.fm/live/1254/64k.mp3"])
        self.assertEqual(stations[index["Jazz FM"]][5:], ["", ""])
        self.assertEqual(finds["交通"], (index["河北交通广播"], "河北交通广播"))
        self.assertEqual(finds["JAZZ"], (index["Jazz FM"], "Jazz FM"))
        self.assertEqual(finds["xgdt"], (index["香港电台第一台"], "香港电台第一台"))
        self.assertEqual(finds["不存在"][0], -1)

    def test_without_a_card_the_small_list_has_the_same_bands(self):
        source, stations, bands, finds = self.run_bands(Path(self.tmp.name) / "missing.bin", "广东", 1262)
        self.assertEqual(source, "source list 3")
        self.assertEqual(bands, {"最近": [1], "本地": [1], "新闻": [0], "音乐": [2], "交通": [1],
                                 "各省": [1], "港澳·海外": [2]})
        self.assertEqual(stations[0][:3], ["15318317", "新闻", "全国 · 新闻"])
        self.assertEqual(finds["JAZZ"], (2, "Swiss Jazz"))
        self.assertEqual(finds["xgdt"][0], -1)


if __name__ == "__main__":
    unittest.main()
