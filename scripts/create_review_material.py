import unreal


ASSET_PATH = "/Game/AAReview/M_SurfaceTextureBase"
TEXTURE_PARAM = "SurfaceTexture"


def ensure_folder(path: str) -> None:
    if unreal.EditorAssetLibrary.does_directory_exist(path):
        return
    unreal.EditorAssetLibrary.make_directory(path)


def create_material():
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    factory = unreal.MaterialFactoryNew()
    package_path, asset_name = ASSET_PATH.rsplit("/", 1)
    material = unreal.load_asset(ASSET_PATH)
    if not material:
        material = asset_tools.create_asset(asset_name, package_path, unreal.Material, factory)
    if not material:
        raise RuntimeError(f"Failed to create material at {ASSET_PATH}")

    unreal.MaterialEditingLibrary.delete_all_material_expressions(material)
    material.set_editor_property("two_sided", True)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)

    tex_expr = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionTextureSampleParameter2D, -400, 0
    )
    tex_expr.set_editor_property("parameter_name", TEXTURE_PARAM)
    tex_expr.set_editor_property(
        "texture",
        unreal.load_asset("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"),
    )
    tex_expr.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)

    brightness_expr = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionScalarParameter, -520, 160
    )
    brightness_expr.set_editor_property("parameter_name", "Brightness")
    brightness_expr.set_editor_property("default_value", 1.8)

    lift_expr = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionScalarParameter, -520, 250
    )
    lift_expr.set_editor_property("parameter_name", "Lift")
    lift_expr.set_editor_property("default_value", 0.12)

    add_expr = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionAdd, -300, 40
    )
    unreal.MaterialEditingLibrary.connect_material_expressions(tex_expr, "RGB", add_expr, "A")
    unreal.MaterialEditingLibrary.connect_material_expressions(lift_expr, "", add_expr, "B")

    mul_expr = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionMultiply, -120, 40
    )
    unreal.MaterialEditingLibrary.connect_material_expressions(add_expr, "", mul_expr, "A")
    unreal.MaterialEditingLibrary.connect_material_expressions(brightness_expr, "", mul_expr, "B")

    unreal.MaterialEditingLibrary.connect_material_property(
        mul_expr, "", unreal.MaterialProperty.MP_BASE_COLOR
    )
    unreal.MaterialEditingLibrary.connect_material_property(
        mul_expr, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR
    )

    unreal.MaterialEditingLibrary.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)
    return material


def main():
    ensure_folder("/Game/AAReview")
    material = create_material()
    unreal.log(f"Updated review material: {material.get_path_name()}")


if __name__ == "__main__":
    main()
