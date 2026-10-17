# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alejandro Bustos
"""Check parsed Turtle against the C++ port ABI. Test dependency: rdflib."""
import pathlib
import re
import sys

try:
    import rdflib
    from rdflib import Namespace, RDF, RDFS, URIRef
except ImportError:
    sys.exit("FAIL ttl_check: install python3-rdflib (or pip install rdflib)")

ROOT = pathlib.Path(__file__).resolve().parents[1]
LV2 = Namespace("http://lv2plug.in/ns/lv2core#")
DOAP = Namespace("http://usefulinc.com/ns/doap#")
PPROPS = Namespace("http://lv2plug.in/ns/ext/port-props#")
UNITS = Namespace("http://lv2plug.in/ns/extensions/units#")
URI = "https://github.com/ABustos2606/discipline#"
OPTIONS = URIRef("http://lv2plug.in/ns/ext/options#options")
MAP = URIRef("http://lv2plug.in/ns/ext/urid#map")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def check(bundle):
    header = (ROOT / "src/ports.hpp").read_text(encoding="utf-8")
    manifest = rdflib.Graph().parse(bundle / "manifest.ttl", format="turtle")
    require(set(manifest.subjects(RDF.type, LV2.Plugin)) ==
            {URIRef(URI + "discipline"), URIRef(URI + "return")}, "manifest descriptors")
    for fragment, enum, filename, count, cls in [
        ("discipline", "MainPort", "discipline.ttl", 35, LV2.DelayPlugin),
        ("return", "ReturnPort", "discipline_return.ttl", 4, LV2.UtilityPlugin),
    ]:
        plugin = URIRef(URI + fragment)
        body = re.search(r"enum class " + enum + r" : uint32_t \{(.*?)\};", header, re.S)
        require(body is not None, f"missing {enum}")
        declared = re.findall(r"(\w+)\s*=\s*(\d+),\s*//\s*(audio_in|audio_out|control_in|control_out|cv_in|cv_out)", body[1])
        expected = {int(index): (symbol, kind) for symbol, index, kind in declared}
        require(len(declared) == len(expected) == count, f"{enum}: count/duplicate index")
        require(set(expected) == set(range(count)), f"{enum}: non-contiguous indices")
        require(re.search(r"count\s*=\s*" + str(count) + r"\b", body[1]), f"{enum}: count sentinel")
        graph = rdflib.Graph().parse(bundle / filename, format="turtle")
        require((plugin, RDF.type, LV2.Plugin) in graph and (plugin, RDF.type, cls) in graph, f"{fragment}: class")
        require(graph.value(plugin, DOAP.license) == URIRef("https://spdx.org/licenses/MIT"), f"{fragment}: license")
        require(set(graph.objects(plugin, LV2.requiredFeature)) == ({MAP} if fragment == "discipline" else set()), f"{fragment}: required features")
        require({LV2.hardRTCapable, OPTIONS} <= set(graph.objects(plugin, LV2.optionalFeature)), f"{fragment}: optional features")
        require(graph.value(plugin, LV2.minorVersion) is not None and graph.value(plugin, LV2.microVersion) is not None, f"{fragment}: version")
        require(manifest.value(plugin, LV2.binary) == URIRef((bundle / "discipline.so").as_uri()), f"{fragment}: shared binary")
        require(manifest.value(plugin, RDFS.seeAlso) == URIRef((bundle / filename).as_uri()), f"{fragment}: seeAlso")
        actual, symbols = {}, set()
        for port in graph.objects(plugin, LV2.port):
            index, symbol, name = int(graph.value(port, LV2["index"])), str(graph.value(port, LV2.symbol)), str(graph.value(port, LV2.name))
            require(index not in actual and symbol not in symbols, f"{fragment}: duplicate port {symbol}")
            symbols.add(symbol)
            require(re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", symbol), f"invalid symbol: {symbol}")
            require(name.isascii() and 0 < len(name) <= 16, f"{symbol}: name must be ASCII, <=16 characters")
            types = set(graph.objects(port, RDF.type))
            directions = types & {LV2.InputPort, LV2.OutputPort}
            kinds = types & {LV2.AudioPort, LV2.ControlPort, LV2.CVPort}
            require(len(directions) == len(kinds) == 1, f"{symbol}: ambiguous port kind")
            kind = {LV2.AudioPort: "audio", LV2.ControlPort: "control", LV2.CVPort: "cv"}[next(iter(kinds))]
            kind += "_in" if LV2.InputPort in types else "_out"
            actual[index] = (symbol, kind)
            if LV2.ControlPort in types:
                lo, hi = graph.value(port, LV2.minimum), graph.value(port, LV2.maximum)
                require(lo is not None and hi is not None and float(lo) <= float(hi), f"{symbol}: bounds")
                if LV2.InputPort in types:
                    default = graph.value(port, LV2.default)
                    require(default is not None and float(lo) <= float(default) <= float(hi), f"{symbol}: default")
            props = set(graph.objects(port, LV2.portProperty))
            if LV2.enumeration in props:
                points = list(graph.objects(port, LV2.scalePoint))
                values = sorted(float(graph.value(p, RDF.value)) for p in points)
                require(LV2.integer in props and values == list(range(len(points))), f"{symbol}: enum values")
                require(values[0] == float(lo) and values[-1] == float(hi), f"{symbol}: enum bounds")
                require(all(str(graph.value(p, RDFS.label)).isascii() for p in points), f"{symbol}: labels")
            if symbol == "pair":
                require(LV2.integer in props and float(lo) == 1 and float(hi) == 16, "Pair range")
            if symbol == "enabled":
                require(LV2.toggled in props and graph.value(port, LV2.designation) == LV2.enabled, "enabled designation")
            if symbol == "clear":
                require(PPROPS.trigger in props, "Clear trigger")
            if symbol == "size":
                require(PPROPS.logarithmic in props and graph.value(port, UNITS.unit) == UNITS.ms, "Size scale/unit")
            if symbol == "size_beats":
                require(graph.value(port, UNITS.unit) == UNITS.beat, "Size beats unit")
        require(actual == expected, f"{fragment}: ports differ from {enum}: {actual}")
    print("PASS ttl_check: 35 Discipline ports, 4 Return ports, features and bundle")


if __name__ == "__main__":
    try:
        check(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "discipline.lv2").resolve())
    except Exception as error:
        sys.exit(f"FAIL ttl_check: {error}")
