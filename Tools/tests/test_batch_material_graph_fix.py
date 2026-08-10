"""Unit tests for Tools/batch_material_graph_fix.py.

Run with either pytest or directly: python test_batch_material_graph_fix.py
"""

from __future__ import annotations

import csv
import json
import tempfile
import unittest
from pathlib import Path

import sys
from pathlib import Path as _Path

TOOLS_DIR = _Path(__file__).resolve().parents[1]
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import batch_material_graph_fix as fix  # noqa: E402

FIXTURES = _Path(__file__).resolve().parent / "fixtures"


def load(path: Path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


class FlaggingTest(unittest.TestCase):
    def test_graph_less_flagged(self):
        self.assertTrue(fix.is_graph_less(load(FIXTURES / "Material_018.json")[0]))

    def test_with_expressions_not_flagged(self):
        export = load(FIXTURES / "Material_018.json")[0]
        export["Properties"]["Expressions"] = [{"ObjectName": "MaterialExpressionConstant_0"}]
        self.assertFalse(fix.is_graph_less(export))

    def test_null_expressions_flagged(self):
        export = load(FIXTURES / "Material_018.json")[0]
        export["Properties"]["Expressions"] = None
        self.assertTrue(fix.is_graph_less(export))


class ExtractionTest(unittest.TestCase):
    def setUp(self):
        self.material_018 = load(FIXTURES / "Material_018.json")[0]
        self.dress = load(FIXTURES / "M_Clothes_Dress.json")[0]
        self.instance = load(FIXTURES / "MI_Clothes_Dress.json")[0]

    def test_uniform_set_located(self):
        uniform = fix.get_uniform_expression_set(self.material_018)
        self.assertIsNotNone(uniform)
        self.assertIn("UniformNumericParameters", uniform)

    def test_loose_numeric_extraction(self):
        uniform = fix.get_uniform_expression_set(self.material_018)
        params = fix.extract_uniform_parameters(uniform)
        kinds = {(p.kind, p.name) for p in params}
        self.assertIn(("Scalar", "Intensity"), kinds)
        self.assertIn(("Vector", "FurTint"), kinds)
        self.assertIn(("Texture", "FurAlbedo"), kinds)

    def test_real_style_extraction(self):
        uniform = fix.get_uniform_expression_set(self.dress)
        params = fix.extract_uniform_parameters(uniform)
        kinds = {(p.kind, p.name) for p in params}
        self.assertIn(("Scalar", "PatternScale"), kinds)
        self.assertIn(("Vector", "DressColor"), kinds)
        self.assertIn(("Texture", "DressAlbedo"), kinds)
        scalar = next(p for p in params if p.name == "PatternScale")
        self.assertAlmostEqual(scalar.default, 0.5)

    def test_function_infos_extracted(self):
        cached = fix.get_cached_expression_data(self.material_018)
        functions = fix.extract_function_infos(cached)
        self.assertTrue(any("MF_PhongToMetalRoughness" in f for f in functions))

    def test_instance_cross_reference(self):
        parent_path, params = fix.instance_parameters(self.instance)
        self.assertEqual(parent_path, "/Game/Characters/Clothes/M_Clothes_Dress")
        names = {p.name for p in params}
        self.assertIn("DressColor", names)
        self.assertIn("DressAlbedo", names)


class RealShapeTest(unittest.TestCase):
    """Coverage for the loose 5.7 dump shape: ParameterType tags, array-of-arrays
    UniformTextureParameters, ReferencedTextures index lookup, string BlendMode,
    FunctionInfos on the MaterialCompilationOutput level."""

    def setUp(self):
        self.real = load(FIXTURES / "M_RealShape.json")[0]

    def test_real_shape_extraction(self):
        uniform = fix.get_uniform_expression_set(self.real)
        params = fix.extract_uniform_parameters(uniform, fix.get_referenced_textures(self.real))
        kinds = {(p.kind, p.name) for p in params}
        self.assertIn(("Vector", "Color main"), kinds)
        self.assertIn(("Scalar", "WPO"), kinds)
        self.assertIn(("Texture", "Color Map"), kinds)
        self.assertIn(("Texture", "Alpha Map"), kinds)

        color_main = next(p for p in params if p.name == "Color main")
        self.assertEqual(color_main.default, {"R": 1.0, "G": 1.0, "B": 1.0, "A": 0.0, "Hex": "FFFFFF"})

        wpo = next(p for p in params if p.name == "WPO")
        self.assertAlmostEqual(wpo.default, 0.2)

        color_map = next(p for p in params if p.name == "Color Map")
        self.assertEqual(color_map.default,
                         "/Game/Character/Clothes/Underwears/Bra_Sexy_001_Color.Bra_Sexy_001_Color")

    def test_real_shape_masked_wiring(self):
        material = fix.MaterialInfo(path="/Game/Test/M_RealShape", name="M_RealShape", export=self.real)
        generated, params, confidence = fix.process_material(material, {}, {})
        self.assertIsNotNone(generated)
        self.assertEqual(confidence, "wired")
        props = self.real["Properties"]
        self.assertIn("BaseColor", props)
        self.assertIn("OpacityMask", props)
        self.assertEqual(props["OpacityMask"]["OutputIndex"], 0)

    def test_compilation_level_function_infos(self):
        material = fix.MaterialInfo(path="/Game/Test/M_RealShape", name="M_RealShape", export=self.real)
        generated, params, confidence = fix.process_material(material, {}, {})
        self.assertTrue(any(e["Type"] == "MaterialExpressionMaterialFunctionCall" for e in generated))
        self.assertIn("Metallic", self.real["Properties"])
        self.assertIn("Roughness", self.real["Properties"])

    def test_loadable_path_normalization(self):
        self.assertEqual(fix.to_loadable_path("/Game/A/Bra_Sexy_001_Color.0"),
                         "/Game/A/Bra_Sexy_001_Color.Bra_Sexy_001_Color")
        self.assertEqual(fix.to_loadable_path("/Game/Textures/T_Dress.T_Dress"),
                         "/Game/Textures/T_Dress.T_Dress")
        self.assertIsNone(fix.to_loadable_path(None))


class SynthesisTest(unittest.TestCase):
    def setUp(self):
        self.material_018 = load(FIXTURES / "Material_018.json")[0]
        self.dress = load(FIXTURES / "M_Clothes_Dress.json")[0]
        self.instance = load(FIXTURES / "MI_Clothes_Dress.json")[0]

    def _info(self, export):
        return fix.MaterialInfo(
            path="/Game/Characters/Fox/Materials",
            name=export["Name"],
            export=export,
        )

    def test_material_018_synthesis(self):
        material = fix.MaterialInfo(path="/Game/Characters/Fox/Materials",
                                    name="Material_018", export=self.material_018)
        generated, params, confidence = fix.process_material(material, {}, {})
        self.assertIsNotNone(generated)
        self.assertGreaterEqual(len(generated), 4)
        types = {e["Type"] for e in generated}
        self.assertIn("MaterialExpressionScalarParameter", types)
        self.assertIn("MaterialExpressionVectorParameter", types)
        self.assertIn("MaterialExpressionTextureSampleParameter2D", types)
        self.assertIn("MaterialExpressionMaterialFunctionCall", types)
        self.assertEqual(confidence, "wired")
        base_color = self.material_018["Properties"]["BaseColor"]
        self.assertEqual(base_color["OutputIndex"], 0)
        self.assertIn("Expression", base_color)

    def test_dress_cross_referenced(self):
        instances = fix.collect_instances([self.instance])
        material = fix.MaterialInfo(path="/Game/Characters/Clothes/M_Clothes_Dress",
                                    name="M_Clothes_Dress", export=self.dress)
        generated, params, confidence = fix.process_material(material, instances, {})
        self.assertIsNotNone(generated)
        self.assertTrue(any(p.source == "instance-cross-referenced" for p in params))
        self.assertEqual(confidence, "wired")

    def test_no_parameters(self):
        empty = json.loads(json.dumps(self.material_018))
        empty["Properties"] = {"BlendMode": "EBlendMode::BLEND_Opaque"}
        empty["LoadedMaterialResources"] = [
            {"LoadedShaderMap": {"Content": {"MaterialCompilationOutput": {"UniformExpressionSet": {}}}}}
        ]
        empty["CachedExpressionData"] = {"FunctionInfos": []}
        material = fix.MaterialInfo(path="/Game/Empty", name="M_Empty", export=empty)
        generated, params, confidence = fix.process_material(material, {}, {})
        self.assertIsNone(generated)
        self.assertEqual(confidence, "no-parameters")


class EndToEndTest(unittest.TestCase):
    def test_run_writes_fixed_files_and_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            input_dir = Path(tmp) / "input"
            output_dir = Path(tmp) / "output"
            for fixture in FIXTURES.glob("*.json"):
                rel = fixture.name
                target = input_dir / rel
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(fixture.read_text(encoding="utf-8"), encoding="utf-8")

            report = Path(tmp) / "report.csv"
            reports = fix.run(input_dir, output_dir, False, report)

            self.assertEqual(len(reports), 3)
            self.assertTrue((output_dir / "Material_018.fixed.json").exists())
            self.assertTrue((output_dir / "M_Clothes_Dress.fixed.json").exists())
            self.assertTrue((output_dir / "M_RealShape.fixed.json").exists())
            self.assertTrue(report.exists())

            patched = load(output_dir / "M_Clothes_Dress.fixed.json")
            types = {e.get("Type") for e in patched}
            self.assertIn("MaterialExpressionVectorParameter", types)
            self.assertIn("MaterialExpressionTextureSampleParameter2D", types)
            material_export = next(e for e in patched if e.get("Type") == "Material")
            self.assertIn("Expressions", material_export["Properties"])
            self.assertEqual(len(material_export["Properties"]["Expressions"]), 3)
            self.assertIn("BaseColor", material_export["Properties"])
            self.assertIn("OpacityMask", material_export["Properties"])

            with open(report, "r", encoding="utf-8", newline="") as handle:
                rows = list(csv.reader(handle))
            self.assertEqual(len(rows), 4)

    def test_dry_run_writes_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            input_dir = Path(tmp) / "input"
            input_dir.mkdir(parents=True)
            for fixture in FIXTURES.glob("*.json"):
                (input_dir / fixture.name).write_text(fixture.read_text(encoding="utf-8"), encoding="utf-8")
            reports = fix.run(input_dir, None, True, None)
            self.assertEqual(len(reports), 3)
            self.assertEqual(list(input_dir.glob("*.fixed.json")), [])


if __name__ == "__main__":
    unittest.main()
