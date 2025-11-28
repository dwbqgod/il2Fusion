package com.tools.module

import android.content.Context
import de.robv.android.xposed.IXposedHookLoadPackage
import de.robv.android.xposed.XC_MethodHook
import de.robv.android.xposed.XposedBridge
import de.robv.android.xposed.XposedHelpers
import de.robv.android.xposed.callbacks.XC_LoadPackage

/**
 * <pre>
 *     author: PenguinAndy
 *     time  : 2025/11/28 17:41
 *     desc  :
 * </pre>
 */
class MainHook: IXposedHookLoadPackage {
    override fun handleLoadPackage(lpparam: XC_LoadPackage.LoadPackageParam) {
        // ignore self
        if (lpparam.packageName == "com.tools.module") return

        XposedBridge.log("[TextExtractTool] Inject to: ${lpparam.packageName}")

        val  cl = lpparam.classLoader

        // Hook Application.attach(Context)
        XposedHelpers.findAndHookMethod(
            "android.app.Application",
            cl,
            "attach",
            Context::class.java,
            object : XC_MethodHook() {
                override fun afterHookedMethod(param: MethodHookParam) {
                    val ctx = param.args[0] as Context

                    // Load native so
                    try {
                        System.loadLibrary("native_hook")
                        XposedBridge.log("[TextExtractTool] native_hook loaded")
                    } catch (e: Throwable) {
                        XposedBridge.log("[TextExtractTool] loadLibrary failed: $e")
                    }

                    // native initialize
                    try {
                        NativeBridge.init()
                        XposedBridge.log("[TextExtractTool] Native init() call")
                    } catch (e: Throwable) {
                        XposedBridge.log("[TextExtractTool] Native init() failed: $e")
                    }

                }
            }
        )
    }
}