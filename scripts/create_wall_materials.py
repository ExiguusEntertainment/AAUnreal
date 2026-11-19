"""Generate the Unreal material parents used by the fixed wall builder."""

import os

import unreal


PACKAGE = "/Game/AAReview"
VARIANTS = (
    ("M_AAWallOpaque", unreal.BlendMode.BLEND_OPAQUE),
    ("M_AAWallMasked", unreal.BlendMode.BLEND_MASKED),
    ("M_AAWallTranslucent", unreal.BlendMode.BLEND_TRANSLUCENT),
)


def connect(editor, material, source, output, target, input_name=""):
    if not editor.connect_material_expressions(source, output, target, input_name):
        raise RuntimeError(f"Could not connect {source.get_name()} to {target.get_name()} in {material.get_name()}")


def connect_property(editor, material, source, output, target):
    if not editor.connect_material_property(source, output, target):
        raise RuntimeError(f"Could not connect {target} in {material.get_name()}")


def native_coordinates(editor, material, coordinates):
    uv_parameters = (
        ("TextureUScale", 1), ("TextureVScale", 1),
        ("TextureUOffset", 0), ("TextureVOffset", 0),
    )
    inputs = []
    for index, (name, default) in enumerate(uv_parameters):
        parameter = editor.create_material_expression(
            material, unreal.MaterialExpressionScalarParameter, -1480, 80 + index * 125
        )
        parameter.set_editor_property("parameter_name", name)
        parameter.set_editor_property("default_value", default)
        parameter.set_editor_property("use_custom_primitive_data", True)
        parameter.set_editor_property("primitive_data_index", index)
        inputs.append(parameter)

    scale = editor.create_material_expression(material, unreal.MaterialExpressionAppendVector, -1200, 80)
    offset = editor.create_material_expression(material, unreal.MaterialExpressionAppendVector, -1200, 320)
    for pair, node in ((inputs[:2], scale), (inputs[2:], offset)):
        connect(editor, material, pair[0], "", node, "A")
        connect(editor, material, pair[1], "", node, "B")
    scaled = editor.create_material_expression(material, unreal.MaterialExpressionMultiply, -820, -240)
    shifted = editor.create_material_expression(material, unreal.MaterialExpressionAdd, -680, -220)
    connect(editor, material, coordinates, "", scaled, "A")
    connect(editor, material, scale, "", scaled, "B")
    connect(editor, material, scaled, "", shifted, "A")
    connect(editor, material, offset, "", shifted, "B")
    return shifted


def review_coordinates(editor, material, coordinates):
    scroll = editor.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -960, -80)
    scroll.set_editor_property("parameter_name", "SurfaceOffset")
    scroll.set_editor_property("default_value", unreal.LinearColor(0, 0, 0, 0))
    scroll_rg = editor.create_material_expression(material, unreal.MaterialExpressionComponentMask, -760, -80)
    scroll_rg.set_editor_property("r", True)
    scroll_rg.set_editor_property("g", True)
    scroll_rg.set_editor_property("b", False)
    scroll_rg.set_editor_property("a", False)
    shifted = editor.create_material_expression(material, unreal.MaterialExpressionAdd, -680, -220)
    connect(editor, material, scroll, "", scroll_rg)
    connect(editor, material, coordinates, "", shifted, "A")
    connect(editor, material, scroll_rg, "", shifted, "B")
    return shifted


def create_wall_material(name, blend_mode, native):
    editor = unreal.MaterialEditingLibrary
    path = f"{PACKAGE}/{name}"
    material = unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
    if material is None:
        material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name, PACKAGE, unreal.Material, unreal.MaterialFactoryNew()
        )
    if material is None:
        raise RuntimeError(f"Could not create {path}")

    editor.delete_all_material_expressions(material)
    material.set_editor_property("blend_mode", blend_mode)
    lit = native and blend_mode != unreal.BlendMode.BLEND_TRANSLUCENT
    material.set_editor_property(
        "shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT if lit else unreal.MaterialShadingModel.MSM_UNLIT
    )
    material.set_editor_property("two_sided", False)

    texture = editor.create_material_expression(material, unreal.MaterialExpressionTextureSampleParameter2D, -540, 0)
    texture.set_editor_property("parameter_name", "SurfaceTexture")
    texture.set_editor_property("texture", unreal.load_asset("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"))
    texture.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)

    coordinates = editor.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -960, -240)
    shifted_uv = native_coordinates(editor, material, coordinates) if native else review_coordinates(editor, material, coordinates)
    # The texture parameter exposes its coordinate pin under a different label
    # from TextureSample. It is the first input on both nodes.
    connect(editor, material, shifted_uv, "", texture)

    color = editor.create_material_expression(material, unreal.MaterialExpressionMultiply, -260, 0)
    connect(editor, material, texture, "RGB", color, "A")
    if native:
        ambient = editor.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -540, 260)
        ambient.set_editor_property("parameter_name", "SectorAmbient")
        ambient.set_editor_property("default_value", 0.15)
        ambient.set_editor_property("use_custom_primitive_data", True)
        ambient.set_editor_property("primitive_data_index", 4)
        connect(editor, material, ambient, "", color, "B")
        if lit:
            connect_property(editor, material, texture, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
            roughness = editor.create_material_expression(material, unreal.MaterialExpressionConstant, -260, 220)
            roughness.set_editor_property("r", 0.85)
            connect_property(editor, material, roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)
    else:
        tint = editor.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -540, 260)
        tint.set_editor_property("parameter_name", "SurfaceTint")
        tint.set_editor_property("default_value", unreal.LinearColor(1, 1, 1, 1))
        connect(editor, material, tint, "", color, "B")
    connect_property(editor, material, color, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    if blend_mode == unreal.BlendMode.BLEND_MASKED:
        connect_property(editor, material, texture, "A", unreal.MaterialProperty.MP_OPACITY_MASK)
    elif blend_mode == unreal.BlendMode.BLEND_TRANSLUCENT:
        strength = editor.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -540, 400)
        strength.set_editor_property("parameter_name", "PaneOpacity")
        strength.set_editor_property("default_value", 0.45)
        opacity = editor.create_material_expression(material, unreal.MaterialExpressionMultiply, -260, 360)
        connect(editor, material, texture, "A", opacity, "A")
        connect(editor, material, strength, "", opacity, "B")
        connect_property(editor, material, opacity, "", unreal.MaterialProperty.MP_OPACITY)

    editor.recompile_material(material)
    if not unreal.EditorAssetLibrary.save_loaded_asset(material, only_if_is_dirty=False):
        raise RuntimeError(f"Could not save {path}")
    unreal.log(f"Saved wall material {path}")


if not unreal.EditorAssetLibrary.does_directory_exist(PACKAGE):
    unreal.EditorAssetLibrary.make_directory(PACKAGE)
native_materials = os.environ.get("AA_NATIVE_WAD_MATERIALS") == "1"
for material_name, mode in VARIANTS:
    name = material_name.replace("AAWall", "AANative") if native_materials else material_name
    create_wall_material(name, mode, native_materials)
