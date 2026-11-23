package com.redmi.rescue;

import android.Manifest;
import android.app.Activity;
import android.app.AppOpsManager;
import android.app.usage.StorageStats;
import android.app.usage.StorageStatsManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Process;
import android.os.storage.StorageManager;
import android.provider.Settings;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

public final class AppStorageAudit {
  private AppStorageAudit(){}
  private static final double GIB=1024.0*1024.0*1024.0;
  private static class AppRow {
    String name,pkg;long bytes,data,code,cache;
    AppRow(String n,String p,long t,long d,long c,long ca) {
      name=n;pkg=p;bytes=t;data=d;code=c;cache=ca;
    }
  }
  private static String gb(long b) {
    return String.format(Locale.forLanguageTag("pt-BR"),"%.2f GB",b/GIB);
  }
  private static boolean permission(Activity a) {
    AppOpsManager ops=(AppOpsManager)a.getSystemService(Context.APP_OPS_SERVICE);
    int mode=ops.checkOpNoThrow(AppOpsManager.OPSTR_GET_USAGE_STATS,Process.myUid(),a.getPackageName());
    return mode==AppOpsManager.MODE_ALLOWED;
  }
  public static void start(Activity a,TextView output) {
    if(!permission(a)) {
      output.setText("O Android precisa de autorização opcional para consultar os tamanhos dos aplicativos.\n\n" +
        "Na tela seguinte, procure Redmi Rescue e autorize 'Acesso ao uso'. Depois volte e toque novamente neste botão.\n\n"+
        "Essa autorização não permite ler conversas, fotos privadas nem apagar aplicativos.");
      try{a.startActivity(new Intent(Settings.ACTION_USAGE_ACCESS_SETTINGS));}
      catch(Exception ex){output.setText("Não consegui abrir configurações. Procure 'Acesso ao uso' nas configurações do Android.");}
      return;
    }
    output.setText("Consultando armazenamento de aplicativos. Não altera nenhum arquivo.");
    new Thread(()->{
      ArrayList<AppRow> stats=new ArrayList<>();
      String error="";
      int skipped=0;
      try{
        PackageManager packages=a.getPackageManager();
        StorageStatsManager manager=(StorageStatsManager)a.getSystemService(Context.STORAGE_STATS_SERVICE);
        List<ApplicationInfo> installed=packages.getInstalledApplications(0);
        for(ApplicationInfo item:installed){
          try {
            StorageStats used=manager.queryStatsForPackage(StorageManager.UUID_DEFAULT,item.packageName,Process.myUserHandle());
            long code=used.getAppBytes();
            long data=used.getDataBytes();
            long cache=used.getCacheBytes();
            long total=code+data;
            if(total<=0)continue;
            String label=item.loadLabel(packages).toString();
            stats.add(new AppRow(label,item.packageName,total,data,code,cache));
          }catch(Exception ex){skipped++;}
        }
      }catch(Exception ex){error=ex.getClass().getSimpleName()+": "+ex.getMessage();}
      stats.sort((x,y)->Long.compare(y.bytes,x.bytes));
      ArrayList<String[]> rows=new ArrayList<>();
      StringBuilder out=new StringBuilder();
      out.append("APLICATIVOS MAIS PESADOS\n\n");
      if(!error.isEmpty())out.append("Falha ao consultar: ").append(error).append("\n");
      if(stats.isEmpty())out.append("Nenhum tamanho disponível: o HyperOS pode estar restringindo esta consulta.\n");
      int shown=Math.min(35,stats.size());
      for(int i=0;i<shown;i++){
        AppRow row=stats.get(i);
        out.append(i+1).append(". ").append(row.name).append(" — ").append(gb(row.bytes))
          .append("\n    Dados: ").append(gb(row.data)).append(" | cache (incluído nos dados): ")
          .append(gb(row.cache)).append(" | aplicativo: ").append(gb(row.code)).append("\n");
      }
      for(AppRow row:stats){
        rows.add(new String[]{"Aplicativo",row.name+" ("+row.pkg+")",
          Long.toString(row.bytes),"1",
          "Dados="+gb(row.data)+", cache="+gb(row.cache)+" (incluído em dados), app="+gb(row.code)});
      }
      out.append("\nApps identificados: ").append(stats.size()).append(" | consultas indisponíveis: ").append(skipped);
      out.append("\nOs totais dos aplicativos podem se sobrepor ao mapa das mídias e não devem ser somados a ele.");
      out.append("\nCache listado não é garantia de que possa ser removido com segurança.");
      out.append("\nNão houve exclusão nem envio de dados.");
      String visible=out.toString();
      a.runOnUiThread(()->{
        output.setText(visible);
        ReportVault.setAppAnalysis(visible,rows);
      });
    },"app-storage-audit").start();
  }
}
