package com.tools.module

/**
 * <pre>
 *     author: PenguinAndy
 *     time  : 2025/11/28 18:10
 *     desc  :
 * </pre>
 */
object NativeBridge {
    private var appContext: android.content.Context? = null

    internal fun setContext(ctx: android.content.Context) {
        appContext = ctx.applicationContext ?: ctx
    }

    @JvmStatic
    fun onDumpFinished(success: Boolean, message: String) {
        val ctx = appContext ?: return
        val text = if (success) "Dump 成功：$message" else "Dump 失败：$message"
        android.os.Handler(android.os.Looper.getMainLooper()).post {
            android.widget.Toast.makeText(ctx, text, android.widget.Toast.LENGTH_SHORT).show()
        }
    }

    @JvmStatic external fun init(processName: String)
    @JvmStatic external fun setTargets(targets: Array<String>)
    @JvmStatic external fun startDump(dataDir: String)
}
