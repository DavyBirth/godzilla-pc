package com.redmi.rescue;

import android.app.Activity;
import android.app.AppOpsManager;
import android.app.usage.ExternalStorageStats;
import android.app.usage.StorageStats;
import android.app.usage.StorageStatsManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.os.Process;
import android.os.storage.StorageManager;
import android.provider.Settings;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

public final class AppStorageAudit {
  private AppStorageAudit(){}
  private static final double GIB=1024.0*1024.0*1024.0;
  private static class AppRow {
    String name,pkg;
    long bytes,data,code,cache;
    AppRow(String n,String p,long t,long d,long c,long ca) {
      name=n;pkg=p;bytes=t;data=d;code=c;cache=ca;
    }
  }
  private static String gb(long b) {
    return String.format(Locale.forLanguageTag("pt-BR"),"%.2f GB",b/GIB);
  }
  public static boolean hasPermission(Activity a) {
    AppOpsManager ops=(AppOpsManager)a.getSystemService(Context.APP_OPS_SERVICE);
    if(ops==null)return false;
    int mode=ops.checkOpNoThrow(AppOpsManager.OPSTR_GET_USAGE_STATS,Process.myUid(),a.getPackageName());
    return mode==AppOpsManager.MODE_ALLOWED;
  }
  public static void requestPermission(Activity a){
    try {a.startActivity(new Intent(Settings.ACTION_USAGE_ACCESS_SETTINGS));}
    catch(Exception ex) { android.widget.Toast.makeText(a,"Abra Configurações > Acesso ao uso.",android.widget.Toast.LENGTH_LONG).show(); }
  }
  public static void start(Activity a,TextView output) {start(a,output,()->{});}
  public static void start(Activity a,TextView output,Runnable onComplete) {
    if(!hasPermission(a)){
      output.setText("Acesso ao uso ainda não concedido. Sem ele, não podemos consultar os dados dos aplicativos.");
      requestPermission(a);
      return;
    }
    output.setText("Consultando aplicativos, dados privados agregados e categorias do Android...");
    new Thread(()->{
      ArrayList<AppRow> stats=new ArrayList<>();
      ArrayList<String[]> rows=new ArrayList<>();
      StringBuilder intro=new StringBuilder();
      String error="";
      int skipped=0,seen=0;
      StorageStatsManager manager=(StorageStatsManager)a.getSystemService(Context.STORAGE_STATS_SERVICE);
      // User-wide statistics cover protected app data that file walking cannot see.
      // This API provides aggregates only; no private file contents are accessed.
      try{
        StorageStats u=manager.queryStatsForUser(StorageManager.UUID_DEFAULT,Process.myUserHandle());
        intro.append("DADOS DO PERFIL (estatísticas do Android)\n");
        intro.append("Dados de usuário/aplicativos: ").append(gb(u.getDataBytes())).append("\n");
        intro.append("Código instalado (pode vir zerado nesta consulta): ").append(gb(u.getAppBytes())).append("\n");
        intro.append("Cache agregado (pode vir zerado nesta consulta): ").append(gb(u.getCacheBytes())).append("\n");
        intro.append("Esses tamanhos NÃO devem ser somados às mídias ou às pastas; podem se sobrepor.\n\n");
        rows.add(new String[]{"Resumo Android","Dados do perfil (agregado)",Long.toString(u.getDataBytes()),"1","Inclui área protegida; não é lixo"});
      }catch(Exception e){
        intro.append("Dados agregados do perfil indisponíveis: ").append(e.getClass().getSimpleName()).append("\n\n");
      }
      // Separate MediaProvider/accounted shared storage estimates, not added to app totals.
      try{
        ExternalStorageStats ext=manager.queryExternalStatsForUser(StorageManager.UUID_DEFAULT,Process.myUserHandle());
        intro.append("CATEGORIAS DE ARMAZENAMENTO COMPARTILHADO (Android)\n");
        intro.append("Total compartilhado reportado: ").append(gb(ext.getTotalBytes())).append("\n");
        intro.append("Fotos: ").append(gb(ext.getImageBytes())).append("\n");
        intro.append("Vídeos: ").append(gb(ext.getVideoBytes())).append("\n");
        intro.append("Áudio: ").append(gb(ext.getAudioBytes())).append("\n");
        intro.append("Arquivos de aplicativos: ").append(gb(ext.getAppBytes())).append("\n");
        intro.append("Categorias podem se sobrepor a outros relatórios; não representam lixo.\n\n");
        rows.add(new String[]{"Resumo Android","Armazenamento compartilhado (estatística)",Long.toString(ext.getTotalBytes()),"1","Estatística Android; não somar com arquivos"});
        rows.add(new String[]{"Resumo Android","Mídia: vídeos",Long.toString(ext.getVideoBytes()),"1","Estatística Android"});
        rows.add(new String[]{"Resumo Android","Mídia: imagens",Long.toString(ext.getImageBytes()),"1","Estatística Android"});
        rows.add(new String[]{"Resumo Android","Mídia: áudio",Long.toString(ext.getAudioBytes()),"1","Estatística Android"});
        rows.add(new String[]{"Resumo Android","Arquivos de aplicativos (armazenamento compartilhado)",Long.toString(ext.getAppBytes()),"1","Categoria agregada Android"});
      }catch(Exception e){
        intro.append("Categorias de armazenamento do Android indisponíveis: ").append(e.getClass().getSimpleName()).append("\n\n");
      }
      try{
        PackageManager packages=a.getPackageManager();
        List<ApplicationInfo> installed=packages.getInstalledApplications(0);
        Set<Integer> seenUids=new HashSet<>();
        for(ApplicationInfo item:installed){
          seen++;
          // Query each UID only once to avoid shared-UID double counting.
          if(!seenUids.add(item.uid))continue;
          try{
            StorageStats used=manager.queryStatsForUid(StorageManager.UUID_DEFAULT,item.uid);
            long code=used.getAppBytes();
            long data=used.getDataBytes();
            long cache=used.getCacheBytes();
            long total=code+data;
            if(total<=0)continue;
            String name=item.loadLabel(packages).toString();
            stats.add(new AppRow(name,item.packageName,total,data,code,cache));
          }catch(Exception e){skipped++;}
        }
      }catch(Exception e){error=e.getClass().getSimpleName()+": "+e.getMessage();}
      stats.sort((x,y)->Long.compare(y.bytes,x.bytes));
      StringBuilder out=new StringBuilder(intro);
      out.append("APLICATIVOS MAIS PESADOS (UIDs únicos)\n\n");
      if(!error.isEmpty())out.append("Erro parcial: ").append(error).append("\n");
      if(stats.isEmpty())out.append("Tamanho por aplicativo indisponível ou retornado como zero pelo HyperOS.\n");
      for(int i=0;i<Math.min(30,stats.size());i++){
        AppRow row=stats.get(i);
        out.append(i+1).append(". ").append(row.name).append(" — ").append(gb(row.bytes))
          .append("\n    Dados: ").append(gb(row.data)).append(" | cache (incluído nos dados): ")
          .append(gb(row.cache)).append(" | aplicativo: ").append(gb(row.code)).append("\n");
      }
      for(AppRow row:stats)rows.add(new String[]{"Aplicativo",
          row.name+" ("+row.pkg+")",Long.toString(row.bytes),"1",
          "Dados="+gb(row.data)+", cache="+gb(row.cache)+" (incluído), código="+gb(row.code)});
      out.append("\nAplicativos encontrados: ").append(seen).append(" | estatísticas individuais: ").append(stats.size())
         .append(" | falhas/indisponíveis: ").append(skipped).append("\n");
      out.append("UIDs compartilhados agrupados. Dados do perfil e individuais podem se sobrepor, NÃO somar.\n");
      out.append("Não foi lido conteúdo privado de aplicativos e nenhum arquivo foi excluído.");
      String result=out.toString();
      a.runOnUiThread(()->{
        output.setText(result);
        ReportVault.setAppAnalysis(result,rows);
        onComplete.run();
      });
    },"app-storage-audit").start();
  }
}
