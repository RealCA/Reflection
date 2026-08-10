#!/usr/bin/env python3
"""Batch scanner/preprocessor for graph-less cooked UMaterial imports in Reflection.

Cooked UMaterial packages never serialize their editor Expressions node graph, so
Reflection imports them as blank material shells. This tool walks a directory of
FModel JSON exports, flags every UMaterial whose Expressions graph is missing or
empty, and writes a copy of each file (`*.fixed.json`) with a synthesized fallback
graph: one expression node per parameter recovered from the compiled output
(LoadedMaterialResources[].Content.MaterialCompilationOutput.UniformExpressionSet)
cross-referenced against any MaterialInstanceConstant in the batch whose Parent
points at the material.

The patched file is shaped so Reflection's existing importer can consume it
unmodified: synthesized expression objects are appended to the top-level export
array (that is where IMaterialGraph::FindMaterialData collects nodes from) and the
material's Properties inputs are wired to them.

Usage:
    python batch_material_graph_fix.py --input <dir> --output <dir> [--dry-run] [--report report.csv]
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

MATERIAL_TYPES = {"Material"}
INSTANCE_TYPES = {"MaterialInstanceConstant"}

EXPRESSION_TYPES = {
    "Scalar": "MaterialExpressionScalarParameter",
    "Vector": "MaterialExpressionVectorParameter",
    "Texture": "MaterialExpressionTextureSampleParameter2D",
}

# Candidate JSON keys for the compiled-output parameter tables. Real FModel dumps name
# these by the UE struct fields (UniformScalarExpressions/UniformVectorExpressions/
# Uniform2DTextureExpressions, with parallel *ParameterValues arrays); the loose
# UniformNumericParameters/UniformTextureParameters names are accepted as well.
UNIFORM_SCALAR_KEYS = ("UniformScalarExpressions",)
UNIFORM_VECTOR_KEYS = ("UniformVectorExpressions",)
UNIFORM_NUMERIC_KEYS = ("UniformNumericParameters",)
UNIFORM_TEXTURE_KEYS = ("Uniform2DTextureExpressions", "UniformTextureExpressions", "UniformTextureParameters")
SCALAR_VALUE_KEYS = ("ScalarParameterValues",)
VECTOR_VALUE_KEYS = ("VectorParameterValues",)
TEXTURE_VALUE_KEYS = ("TextureParameterValues",)

# Hardcoded function lookup table for the function-call heuristic. Expand as needed.
KNOWN_FUNCTIONS = {
    "MF_PhongToMetalRoughness": "ToMetalRoughness",
}

# Material output pins this heuristic is willing to drive, in preference order.
COLOR_TOKENS = ("color", "albedo", "diffuse", "base", "diff")
OPACITY_TOKENS = ("alpha", "mask", "opacity", "opac", "ao")


@dataclass
class Parameter:
    name: str
    kind: str
    default: Any
    source: str
    expression_guid: Optional[str] = None


@dataclass
class MaterialInfo:
    path: str
    name: str
    export: Dict[str, Any]
    functions: List[str] = field(default_factory=list)
    connected_mask: Optional[int] = None


@dataclass
class ScanReport:
    file: str
    material_path: str
    param_count: int
    source: str
    functions: str
    confidence: str


def normalize_object_path(value: Any) -> Optional[str]:
    """Pull a package path out of the several object-ref shapes FModel uses."""
    if value is None:
        return None
    if isinstance(value, dict):
        for key in ("ObjectPath", "ObjectName", "AssetPathName"):
            if value.get(key):
                return value[key]
        return None
    if isinstance(value, str):
        return value
    return None


def path_to_package(object_path: Optional[str]) -> Optional[str]:
    """'Class '/Game/A/B.B:SubObj'' or '/Game/A/B.B' -> '/Game/A/B'."""
    if not object_path:
        return None
    path = object_path
    if "'" in path:
        start = path.find("'") + 1
        end = path.rfind("'")
        if end > start:
            path = path[start:end]
    if ":" in path:
        path = path.split(":", 1)[0]
    if "." in path:
        path = path.rsplit(".", 1)[0]
    return path


def parse_bool(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        return value.strip().lower() in ("true", "1", "yes")
    if isinstance(value, (int, float)):
        return bool(value)
    return False


def is_graph_less(export: Dict[str, Any]) -> bool:
    props = export.get("Properties")
    if not isinstance(props, dict):
        return True
    expressions = props.get("Expressions")
    return expressions is None or expressions == [] or expressions == ""


def first_array(obj: Dict[str, Any], keys: Tuple[str, ...]) -> List[Any]:
    for key in keys:
        value = obj.get(key)
        if isinstance(value, list):
            return value
    return []


def get_loaded_content(export: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    """First LoadedMaterialResources entry's Content object.

    Real FModel dumps keep LoadedMaterialResources at the export top level, each
    entry wrapping its compiled data in LoadedShaderMap.Content; older shapes nest
    LoadedMaterialResources under Properties with a direct Content. Accept all three."""
    props = export.get("Properties")
    props = props if isinstance(props, dict) else {}
    for source in (export, props):
        resources = source.get("LoadedMaterialResources")
        if not isinstance(resources, list) or not resources:
            continue
        resource = resources[0]
        if not isinstance(resource, dict):
            continue
        content = resource.get("Content")
        if isinstance(content, dict):
            return content
        shader_map = resource.get("LoadedShaderMap")
        if isinstance(shader_map, dict) and isinstance(shader_map.get("Content"), dict):
            return shader_map["Content"]
    return None


def get_uniform_expression_set(export: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    content = get_loaded_content(export)
    if not content:
        return None
    compilation = content.get("MaterialCompilationOutput")
    if not isinstance(compilation, dict):
        return None
    return compilation.get("UniformExpressionSet")


def get_compilation_output(export: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    """MaterialCompilationOutput object (FunctionInfos/PropertyConnectedMask live
    there in real FModel dumps, inside UniformExpressionSet in older ones)."""
    content = get_loaded_content(export)
    if not content:
        return None
    compilation = content.get("MaterialCompilationOutput")
    return compilation if isinstance(compilation, dict) else None


def get_cached_expression_data(export: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    """Top-level CachedExpressionData: FunctionInfos/PropertyConnectedMask and a
    parallel ReferencedTextures list live here in real 5.x dumps."""
    ced = export.get("CachedExpressionData")
    return ced if isinstance(ced, dict) else None


def get_referenced_textures(export: Dict[str, Any]) -> List[Dict[str, Any]]:
    """ReferencedTextures: real exports index texture parameters into this list.
    Found at the export top level, under CachedExpressionData, or in the first
    LoadedMaterialResources entry's Content."""
    refs = export.get("ReferencedTextures")
    if isinstance(refs, list):
        return refs
    ced = get_cached_expression_data(export)
    if ced and isinstance(ced.get("ReferencedTextures"), list):
        return ced["ReferencedTextures"]
    content = get_loaded_content(export)
    if content and isinstance(content.get("ReferencedTextures"), list):
        return content["ReferencedTextures"]
    return []


def to_loadable_path(object_path: Optional[str]) -> Optional[str]:
    """'/Game/Path/Asset.0' -> '/Game/Path/Asset.Asset' (FSoftObjectPath-friendly)."""
    if not object_path:
        return None
    path = object_path
    stem = path.rsplit(".", 1)
    if len(stem) == 2 and stem[1].isdigit():
        path = stem[0]
    name = path.rsplit("/", 1)[-1].split(".", 1)[0]
    if not name:
        return path
    if path.rsplit(".", 1)[-1] != name:
        path = path + "." + name
    return path


def parameter_name(entry: Any) -> Optional[str]:
    if isinstance(entry, str):
        return entry
    if not isinstance(entry, dict):
        return None
    for key in ("Name", "ParameterName"):
        value = entry.get(key)
        if isinstance(value, str) and value:
            return value
    info = entry.get("ParameterInfo")
    if isinstance(info, dict) and isinstance(info.get("Name"), str):
        return info["Name"]
    return None


def parameter_value(entry: Any, parallel: Optional[List[Any]], index: int, kind: str) -> Any:
    candidates = []
    if parallel and index < len(parallel):
        candidates.append(parallel[index])
    candidates.append(entry)
    for candidate in candidates:
        if candidate is None:
            continue
        if not isinstance(candidate, dict):
            # Parallel value arrays (e.g. ScalarParameterValues) hold bare numbers
            return candidate
        for key in ("Value", "DefaultValue", "ParameterValue"):
            if key in candidate:
                return candidate[key]
    return None


def numeric_entry_kind(entry: Any) -> Optional[str]:
    """'Scalar'/'Vector' for a numeric parameter entry, or None when undecidable."""
    if not isinstance(entry, dict):
        return None
    kind = entry.get("Type") or entry.get("ParameterType")
    if isinstance(kind, str):
        lowered = kind.lower()
        if "scalar" in lowered or "float" in lowered:
            return "Scalar"
        if "vector" in lowered or "linear" in lowered or "float4" in lowered:
            return "Vector"
    return None


def extract_numeric_parameters(uniform: Dict[str, Any]) -> List[Parameter]:
    params: List[Parameter] = []
    scalar_values = first_array(uniform, SCALAR_VALUE_KEYS)
    vector_values = first_array(uniform, VECTOR_VALUE_KEYS)

    scalar_index = 0
    vector_index = 0
    for entry in first_array(uniform, UNIFORM_SCALAR_KEYS):
        kind = numeric_entry_kind(entry) or "Scalar"
        if kind != "Scalar":
            continue
        name = parameter_name(entry)
        if name:
            params.append(Parameter(name=name, kind="Scalar",
                                    default=parameter_value(entry, scalar_values, scalar_index, "Scalar"),
                                    source="uniform-table"))
        scalar_index += 1

    for entry in first_array(uniform, UNIFORM_VECTOR_KEYS):
        kind = numeric_entry_kind(entry) or "Vector"
        if kind != "Vector":
            continue
        name = parameter_name(entry)
        if name:
            params.append(Parameter(name=name, kind="Vector",
                                    default=parameter_value(entry, vector_values, vector_index, "Vector"),
                                    source="uniform-table"))
        vector_index += 1

    return params


def extract_uniform_parameters(uniform: Dict[str, Any],
                               referenced_textures: Optional[List[Dict[str, Any]]] = None) -> List[Parameter]:
    params: List[Parameter] = []
    numeric = first_array(uniform, UNIFORM_NUMERIC_KEYS)
    if numeric:
        # Loose "UniformNumericParameters" form: entries carry their own Type tag
        scalar_values = first_array(uniform, SCALAR_VALUE_KEYS)
        vector_values = first_array(uniform, VECTOR_VALUE_KEYS)
        scalar_index = 0
        vector_index = 0
        for entry in numeric:
            kind = numeric_entry_kind(entry)
            if kind == "Vector":
                name = parameter_name(entry)
                if name:
                    params.append(Parameter(name=name, kind="Vector",
                                            default=parameter_value(entry, vector_values, vector_index, "Vector"),
                                            source="uniform-table"))
                vector_index += 1
            else:
                kind = kind or "Scalar"
                name = parameter_name(entry)
                if name:
                    params.append(Parameter(name=name, kind=kind,
                                            default=parameter_value(entry, scalar_values, scalar_index, "Scalar"),
                                            source="uniform-table"))
                scalar_index += 1
    else:
        params.extend(extract_numeric_parameters(uniform))

    texture_values = first_array(uniform, TEXTURE_VALUE_KEYS)
    index = 0
    # UniformTextureParameters is a per-sampler-type array of arrays; flatten it.
    for bucket in first_array(uniform, UNIFORM_TEXTURE_KEYS):
        entries = bucket if isinstance(bucket, list) else [bucket]
        for entry in entries:
            name = parameter_name(entry)
            if name:
                value = normalize_object_path(parameter_value(entry, texture_values, index, "Texture"))
                if not value and isinstance(entry, dict):
                    texture_index = entry.get("TextureIndex")
                    if isinstance(texture_index, int) and referenced_textures and 0 <= texture_index < len(referenced_textures):
                        value = normalize_object_path(referenced_textures[texture_index])
                params.append(Parameter(name=name, kind="Texture",
                                        default=to_loadable_path(value),
                                        source="uniform-table"))
            index += 1

    return params


def extract_function_infos(uniform: Dict[str, Any]) -> List[str]:
    functions: List[str] = []
    for entry in uniform.get("FunctionInfos", []) or []:
        if not isinstance(entry, dict):
            continue
        function = entry.get("Function")
        path = normalize_object_path(function)
        if path:
            functions.append(path)
    return functions


def instance_parameters(instance: Dict[str, Any]) -> Tuple[str, List[Parameter]]:
    props = instance.get("Properties", {})
    parent = props.get("Parent")
    parent_path = path_to_package(normalize_object_path(parent))
    params: List[Parameter] = []

    for key, kind, value_keys in (
        ("ScalarParameterValues", "Scalar", SCALAR_VALUE_KEYS),
        ("VectorParameterValues", "Vector", VECTOR_VALUE_KEYS),
        ("TextureParameterValues", "Texture", TEXTURE_VALUE_KEYS),
    ):
        for value_key in value_keys:
            for entry in props.get(value_key, []) or []:
                if not isinstance(entry, dict):
                    continue
                name = parameter_name(entry)
                if not name:
                    continue
                value = entry.get("ParameterValue")
                if kind == "Texture":
                    value = normalize_object_path(value)
                params.append(Parameter(name=name, kind=kind, default=value, source="instance"))
            break

    return parent_path, params


def collect_instances(exports: List[Dict[str, Any]]) -> Dict[str, List[Parameter]]:
    by_parent: Dict[str, List[Parameter]] = {}
    blend_modes: Dict[str, str] = {}
    for export in exports:
        if export.get("Type") not in INSTANCE_TYPES:
            continue
        parent_path, params = instance_parameters(export)
        if parent_path:
            by_parent.setdefault(parent_path, []).extend(params)
        props = export.get("Properties", {})
        overrides = props.get("BasePropertyOverrides")
        if isinstance(overrides, dict) and isinstance(overrides.get("BlendMode"), str):
            blend_modes[parent_path or ""] = overrides["BlendMode"]
    return by_parent


def merge_parameters(uniform: List[Parameter], instances: List[Parameter]) -> List[Parameter]:
    by_name: Dict[Tuple[str, str], Parameter] = {}
    for param in uniform:
        by_name[(param.kind, param.name)] = param
    for param in instances:
        existing = by_name.get((param.kind, param.name))
        if existing is None:
            by_name[(param.kind, param.name)] = param
        else:
            if param.default is not None:
                existing.default = param.default
            existing.source = "instance-cross-referenced"
    return list(by_name.values())


def stable_guid(seed: str) -> str:
    digest = uuid.uuid5(uuid.NAMESPACE_OID, seed)
    return str(digest).upper()


def expression_guid_for(param: Parameter, material_name: str) -> str:
    return stable_guid(f"{material_name}/{param.kind}/{param.name}")


def color_from_default(default: Any) -> Dict[str, float]:
    if isinstance(default, dict):
        return {
            "R": default.get("R", 0.0),
            "G": default.get("G", 0.0),
            "B": default.get("B", 0.0),
            "A": default.get("A", 1.0),
        }
    return {"R": 0.0, "G": 0.0, "B": 0.0, "A": 1.0}


def scalar_from_default(default: Any) -> float:
    if isinstance(default, (int, float)):
        return float(default)
    if isinstance(default, dict):
        return float(default.get("R", default.get("Value", 0.0)) or 0.0)
    return 0.0


def build_expression_export(kind: str, name: str, param: Parameter, index: int,
                            material_path: str, material_name: str) -> Dict[str, Any]:
    expr_name = f"{EXPRESSION_TYPES[kind]}_{index}"
    outer = f"{material_path}.{material_name}:{expr_name}"
    object_path = f"{material_path}.{expr_name}"
    props: Dict[str, Any] = {
        "ParameterName": param.name,
        "Group": "",
        "ExpressionGUID": expression_guid_for(param, material_name),
        "MaterialExpressionGuid": expression_guid_for(param, material_name),
        "MaterialExpressionEditorX": index * 150,
        "MaterialExpressionEditorY": 0,
    }
    if kind == "Scalar":
        props["DefaultValue"] = scalar_from_default(param.default)
    elif kind == "Vector":
        props["DefaultValue"] = color_from_default(param.default)
    else:
        texture_path = to_loadable_path(param.default) if isinstance(param.default, str) else object_path
        if not texture_path:
            texture_path = object_path
        props["Texture"] = {
            "ObjectName": f"Texture2D '{texture_path}'",
            "ObjectPath": texture_path,
        }
        props["SamplerType"] = "SAMPLERTYPE_Color"
    return {
        "Type": EXPRESSION_TYPES[kind],
        "Name": expr_name,
        "Outer": {"ObjectName": f"Material '{outer}'", "ObjectPath": outer},
        "Class": EXPRESSION_TYPES[kind],
        "Properties": props,
    }


def build_function_export(function_path: str, index: int, material_path: str,
                          material_name: str) -> Dict[str, Any]:
    expr_name = f"MaterialExpressionMaterialFunctionCall_{index}"
    outer = f"{material_path}.{material_name}:{expr_name}"
    return {
        "Type": "MaterialExpressionMaterialFunctionCall",
        "Name": expr_name,
        "Outer": {"ObjectName": f"Material '{outer}'", "ObjectPath": outer},
        "Class": "MaterialExpressionMaterialFunctionCall",
        "Properties": {
            "MaterialFunction": {"ObjectName": f"MaterialFunction '{function_path}'",
                                 "ObjectPath": function_path},
            "MaterialExpressionEditorX": index * 150,
            "MaterialExpressionEditorY": 100,
        },
    }


def expression_ref(expr: Dict[str, Any], material_path: str) -> Dict[str, str]:
    return {
        "ObjectName": f"MaterialExpression '{material_path}.{expr['Name']}'",
        "ObjectPath": f"{material_path}.{expr['Name']}",
        "Outer": material_path,
    }


def function_output_name(function_path: str) -> Optional[str]:
    base = function_path.rsplit("/", 1)[-1].split(".")[0]
    for key, token in KNOWN_FUNCTIONS.items():
        if key in function_path or base == key or token in base:
            return token
    return None


def is_colorish(name: str) -> bool:
    lowered = name.lower()
    return any(token in lowered for token in COLOR_TOKENS)


def is_opacity(name: str) -> bool:
    lowered = name.lower()
    return any(token in lowered for token in OPACITY_TOKENS)


def wire_material_inputs(props: Dict[str, Any], material_path: str, params: List[Parameter],
                         expressions: List[Dict[str, Any]], functions: List[Dict[str, Any]],
                         is_masked: bool) -> List[str]:
    """Wire synthesized nodes to material output pins using the v1 heuristic.

    Returns the list of pins driven, so the report can say whether the fallback
    graph is "wired" or only "unwired-fallback"."""
    by_expr = {expr["Properties"]["ParameterName"]: expr for expr in expressions}
    wired: List[str] = []

    def connect(pin: str, expr: Dict[str, Any], output_index: int = 0) -> None:
        props[pin] = {
            "Expression": expression_ref(expr, material_path),
            "OutputIndex": output_index,
            "InputName": pin,
            "Mask": 0,
            "MaskR": 1, "MaskG": 1, "MaskB": 1, "MaskA": 1,
        }
        wired.append(pin)

    color_pin = "BaseColor"
    for param in params:
        if param.kind != "Texture":
            continue
        expr = by_expr.get(param.name)
        if expr is None:
            continue
        if color_pin not in wired and is_colorish(param.name):
            connect(color_pin, expr)
        elif is_masked and is_opacity(param.name) and "OpacityMask" not in wired:
            connect("OpacityMask", expr)

    for function in functions:
        token = function_output_name(function["Properties"]["MaterialFunction"]["ObjectPath"])
        if token and "ToMetalRoughness" in token:
            connect("Metallic", function, 0)
            connect("Roughness", function, 1)
            break

    return wired


def material_is_masked(props: Dict[str, Any]) -> bool:
    """BlendMode may serialize as a number (1 = BLEND_Masked), a full enum string
    ("EBlendMode::BLEND_Masked") or live under BasePropertyOverrides."""
    blend = props.get("BlendMode")
    if isinstance(blend, (int, float)):
        return blend == 1
    if isinstance(blend, str):
        return "Masked" in blend
    overrides = props.get("BasePropertyOverrides")
    if isinstance(overrides, dict):
        override_blend = overrides.get("BlendMode")
        if isinstance(override_blend, str) and "Masked" in override_blend:
            return True
    return False


def process_material(material: MaterialInfo, instances_by_parent: Dict[str, List[Parameter]],
                     blend_modes: Dict[str, str]) -> Tuple[Optional[List[Dict[str, Any]]], List[Parameter], str]:
    uniform = get_uniform_expression_set(material.export)
    compilation = get_compilation_output(material.export)
    cached = get_cached_expression_data(material.export)
    if isinstance(uniform, dict):
        params = extract_uniform_parameters(uniform, get_referenced_textures(material.export))
        if cached and "FunctionInfos" in cached:
            functions = extract_function_infos(cached)
        elif isinstance(compilation, dict) and "FunctionInfos" in compilation:
            functions = extract_function_infos(compilation)
        else:
            functions = extract_function_infos(uniform)
        material.functions = functions
        connected_mask = None
        if cached and isinstance(cached.get("PropertyConnectedMask"), (int, float)):
            connected_mask = cached["PropertyConnectedMask"]
        elif isinstance(compilation, dict) and isinstance(compilation.get("PropertyConnectedMask"), (int, float)):
            connected_mask = compilation["PropertyConnectedMask"]
        elif isinstance(uniform.get("PropertyConnectedMask"), (int, float)):
            connected_mask = uniform["PropertyConnectedMask"]
        if isinstance(connected_mask, (int, float)):
            material.connected_mask = int(connected_mask)
    else:
        params, functions = [], []

    instance_params = instances_by_parent.get(material.path, [])
    params = merge_parameters(params, instance_params)

    expressions: List[Dict[str, Any]] = []
    for index, param in enumerate(params):
        expressions.append(build_expression_export(
            param.kind, param.name, param, index, material.path, material.name))

    function_exports: List[Dict[str, Any]] = []
    for index, function_path in enumerate(material.functions):
        if function_output_name(function_path):
            function_exports.append(build_function_export(
                function_path, index, material.path, material.name))

    if not expressions and not function_exports:
        return None, params, "no-parameters"

    blend = blend_modes.get(material.path, "")
    is_masked = material_is_masked(material.export.get("Properties", {})) or "BLEND_Masked" in blend
    wired = wire_material_inputs(material.export.setdefault("Properties", {}),
                                 material.path, params, expressions, function_exports, is_masked)

    material.export["Properties"].setdefault("Expressions", [])
    for expr in expressions + function_exports:
        material.export["Properties"]["Expressions"].append({
            "ObjectName": f"{expr['Class']} '{material.path}.{expr['Name']}'",
            "ObjectPath": f"{material.path}.{expr['Name']}",
            "Outer": material.path,
        })

    return expressions + function_exports, params, "wired" if wired else "unwired-fallback"


def load_exports(path: Path) -> List[Dict[str, Any]]:
    try:
        with open(path, "r", encoding="utf-8") as handle:
            data = json.load(handle)
    except (json.JSONDecodeError, OSError) as error:
        print(f"[warn] {path.name}: cannot parse ({error})", file=sys.stderr)
        return []
    if isinstance(data, list):
        return data
    if isinstance(data, dict):
        exports = data.get("exports")
        if isinstance(exports, list):
            return exports
    return []


def scan_directory(input_dir: Path) -> List[Tuple[Path, List[Dict[str, Any]]]]:
    files: List[Tuple[Path, List[Dict[str, Any]]]] = []
    for path in sorted(input_dir.rglob("*.json")):
        exports = load_exports(path)
        if exports:
            files.append((path, exports))
    return files


def run(input_dir: Path, output_dir: Path, dry_run: bool, report_path: Optional[Path]) -> List[ScanReport]:
    reports: List[ScanReport] = []
    materials: List[Tuple[Path, MaterialInfo]] = []
    all_exports: List[Tuple[Path, List[Dict[str, Any]]]] = scan_directory(input_dir)

    for file_path, exports in all_exports:
        for export in exports:
            if export.get("Type") not in MATERIAL_TYPES:
                continue
            props = export.get("Properties", {})
            if not is_graph_less(export):
                continue
            raw_package = export.get("Package")
            if isinstance(raw_package, str):
                package_path = raw_package
            else:
                outer = props.get("Outer")
                package_path = path_to_package(normalize_object_path(outer)) or path_to_package(
                    normalize_object_path(props.get("ObjectPath"))) or ""
            name = export.get("Name", "")
            if package_path:
                if not package_path.endswith("/" + name):
                    package_path = package_path + "/" + name
            else:
                package_path = f"/Game/{name}"
            materials.append((file_path, MaterialInfo(
                path=package_path, name=name, export=export)))

    instances_by_parent: Dict[str, List[Parameter]] = {}
    blend_modes: Dict[str, str] = {}
    for _, exports in all_exports:
        for export in exports:
            if export.get("Type") not in INSTANCE_TYPES:
                continue
            parent_path, params = instance_parameters(export)
            if parent_path:
                instances_by_parent.setdefault(parent_path, []).extend(params)
            overrides = export.get("Properties", {}).get("BasePropertyOverrides")
            if isinstance(overrides, dict) and isinstance(overrides.get("BlendMode"), str):
                blend_modes[parent_path] = overrides["BlendMode"]

    patched: Dict[Path, List[Dict[str, Any]]] = {}
    for file_path, material in materials:
        generated, params, confidence = process_material(material, instances_by_parent, blend_modes)
        source = "none"
        if params:
            sources = {p.source.split("-")[0] for p in params}
            source = "+".join(sorted(sources))
        reports.append(ScanReport(
            file=file_path.name,
            material_path=material.path,
            param_count=len(params),
            source=source,
            functions=";".join(material.functions),
            confidence=confidence if generated else "no-parameters",
        ))
        if generated is not None and not dry_run:
            patched.setdefault(file_path, []).extend(generated)

    if not dry_run:
        exports_by_file = dict(all_exports)
        for file_path, generated in patched.items():
            exports = exports_by_file[file_path]
            existing_names = {e.get("Name") for e in exports}
            for expr in generated:
                if expr["Name"] not in existing_names:
                    exports.append(expr)
            relative = file_path.relative_to(input_dir)
            target = output_dir / relative.parent / f"{file_path.stem}.fixed.json"
            target.parent.mkdir(parents=True, exist_ok=True)
            with open(target, "w", encoding="utf-8") as handle:
                json.dump(exports, handle, indent=1)

    if report_path is not None:
        report_path.parent.mkdir(parents=True, exist_ok=True)
        with open(report_path, "w", encoding="utf-8", newline="") as handle:
            writer = csv.writer(handle)
            writer.writerow(["file", "material_path", "param_count", "source",
                             "functions", "confidence"])
            for report in reports:
                writer.writerow([report.file, report.material_path, report.param_count,
                                 report.source, report.functions, report.confidence])

    return reports


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Batch-fix graph-less cooked UMaterial JSON exports.")
    parser.add_argument("--input", required=True, help="Root directory of FModel JSON exports.")
    parser.add_argument("--output", help="Directory to write *.fixed.json copies (required unless --dry-run).")
    parser.add_argument("--dry-run", action="store_true", help="Scan and report only; write no files.")
    parser.add_argument("--report", help="Optional CSV report path.")
    args = parser.parse_args(argv)

    input_dir = Path(args.input)
    if not input_dir.is_dir():
        print(f"Input directory not found: {input_dir}", file=sys.stderr)
        return 2
    output_dir = Path(args.output) if args.output else None
    if not args.dry_run and output_dir is None:
        print("--output is required unless --dry-run is set.", file=sys.stderr)
        return 2

    reports = run(input_dir, output_dir, args.dry_run, Path(args.report) if args.report else None)

    print(f"Scanned: {input_dir}")
    print(f"Graph-less materials found: {len(reports)}")
    for report in reports:
        print(f"  {report.material_path:60s} params={report.param_count:3d} "
              f"source={report.source:22s} confidence={report.confidence}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
