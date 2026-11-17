fn main() {
    println!("cargo:rerun-if-env-changed=MODELD_NATIVE_LIB_DIR");
    println!("cargo:rerun-if-env-changed=TENSORRT_LIB_DIR");
    println!("cargo:rerun-if-env-changed=CUDA_LIB_DIR");
    let native = std::env::var("MODELD_NATIVE_LIB_DIR")
        .expect("Build the CMake MODELD_SERVER target, then set MODELD_NATIVE_LIB_DIR");
    for library in ["modeld_native", "modeld_rife", "modeld_core"] {
        println!("cargo:rerun-if-changed={native}/lib{library}.a");
    }
    let trt = std::env::var("TENSORRT_LIB_DIR").unwrap_or("/opt/tensorrt-rtx/lib".into());
    let cuda = std::env::var("CUDA_LIB_DIR").unwrap_or("/opt/cuda/lib64".into());
    for directory in [native, trt, cuda] {
        println!("cargo:rustc-link-search=native={directory}");
    }
    for library in ["modeld_native", "modeld_rife", "modeld_core"] {
        println!("cargo:rustc-link-lib=static={library}");
    }
    for library in [
        "avformat",
        "avcodec",
        "avutil",
        "swscale",
        "tensorrt_onnxparser_rtx",
        "tensorrt_rtx",
        "cudart",
        "cuda",
        "crypto",
        "stdc++",
    ] {
        println!("cargo:rustc-link-lib=dylib={library}");
    }
}
