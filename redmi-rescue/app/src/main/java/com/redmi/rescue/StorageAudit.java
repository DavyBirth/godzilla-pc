package com.redmi.rescue;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.os.PowerManager;
import android.provider.Settings;
import android.widget.TextView;
import java.io.File;
import java.nio.file.Files;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;
import java.util.PriorityQueue;

public final class StorageAudit {
  private StorageAudit() {}
  private static final long GiB=1024L*1024L*1024L;
  private static final int MAX_FILES=120000, MAX_DEPTH=20;
  private static class Dir {
    final File file; final int depth;
    Dir(File f,int d){file=f;depth=d;}
  }
  private static class Big {
    String name, category; long bytes;
    Big(String n,String c,long b){name=n;category=c;bytes=b;}
  }
  private static String gb(long n){return String.format(Locale.forLanguageTag("pt-BR"),"%.2f GB",n/(double)GiB);}
  static void askPermission(Activity activity) {
    if (Build.VERSION.SDK_INT<30) return;
    try {
      Intent i=new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
         Uri.parse("package:"+activity.getPackageName()));
      activity.startActivity(i);
    } catch(Exception e) {
      activity.startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
    }
  }
  static void start(Activity a,TextView output,java.util.function.Consumer<String> onReport) {
    if (Build.VERSION.SDK_INT>=30 && !Environment.isExternalStorageManager()) {
      output.setText("Para mapear pastas e downloads, autorize opcionalmente 'Acesso a todos os arquivos' nas configurações. Nenhum arquivo será excluído. Você pode revogar a permissão depois. O diagnóstico básico funciona sem essa permissão.");
      askPermission(a);
      return;
    }
    output.setText("Lendo tamanhos de arquivos no armazenamento compartilhado. Sem analisar conteúdo e sem excluir. Pode demorar em aparelhos cheios.");
    new Thread(()-> {
      File base=Environment.getExternalStorageDirectory();
      ArrayDeque<Dir> queue=new ArrayDeque<>();
      queue.add(new Dir(base,0));
      Map<String,Long> totals=new HashMap<>();
      Map<String,Integer> counts=new HashMap<>();
      PriorityQueue<Big> biggest=new PriorityQueue<>((x,y)->Long.compare(x.bytes,y.bytes));
      int fileCount=0,skipped=0,folders=0,candidates=0;
      long visibleBytes=0, candidateBytes=0, videoBytes=0;
      boolean stoppedThermal=false;
      PowerManager pm=(PowerManager)a.getSystemService(Activity.POWER_SERVICE);
      while(!queue.isEmpty() && fileCount<MAX_FILES && folders<50000) {
        Dir here=queue.pollFirst(); folders++;
        if(folders%150==0) {
          int thermal=Build.VERSION.SDK_INT>=29?pm.getCurrentThermalStatus():0;
          if(thermal>=PowerManager.THERMAL_STATUS_SEVERE){stoppedThermal=true;break;}
          final int n=fileCount, d=folders;
          a.runOnUiThread(()->output.setText("Mapeando... "+n+" arquivos, "+d+" pastas. Leitura apenas."));
        }
        File[] children;
        try{children=here.file.listFiles();}catch(Exception e){children=null;}
        if(children==null){skipped++;continue;}
        for(File f:children){
          if(fileCount>=MAX_FILES)break;
          try{
            if(Files.isSymbolicLink(f.toPath())){skipped++;continue;}
            String path=f.getAbsolutePath();
            if(f.isDirectory()) {
              if(here.depth>=MAX_DEPTH ||
                 path.contains("/Android/data") || path.contains("/Android/obb")) {
                skipped++;continue;
              }
              queue.addLast(new Dir(f,here.depth+1));
              continue;
            }
            if(!f.isFile())continue;
            long bytes=f.length();
            fileCount++; if(bytes<=0)continue;
            visibleBytes+=bytes;
            String rel=path.substring(Math.min(path.length(),base.getAbsolutePath().length()));
            if(rel.startsWith("/"))rel=rel.substring(1);
            String[] seg=rel.split("/");
            String category=seg.length==0?"Outros":seg[0];
            if("Android".equalsIgnoreCase(category) && seg.length>1) {
              category="Android/"+seg[1];
              if(seg.length>2 && "media".equalsIgnoreCase(seg[1]))category+="/"+seg[2];
            }
            totals.put(category,totals.getOrDefault(category,0L)+bytes);
            counts.put(category,counts.getOrDefault(category,0)+1);
            if(biggest.size()<20)biggest.offer(new Big(f.getName(),category,bytes));
            else if(bytes>biggest.peek().bytes){biggest.poll();biggest.offer(new Big(f.getName(),category,bytes));}
            String lower=f.getName().toLowerCase(Locale.ROOT);
            if(lower.endsWith(".mp4")||lower.endsWith(".mkv")||lower.endsWith(".mov")||lower.endsWith(".avi"))videoBytes+=bytes;
            boolean isTemp=lower.endsWith(".apk")||lower.endsWith(".apks")||lower.endsWith(".xapk")||
                 lower.endsWith(".crdownload")||lower.endsWith(".part")||lower.endsWith(".tmp");
            String p=path.toLowerCase(Locale.ROOT);
            if(isTemp && !p.contains("whatsapp")&&!p.contains("telegram")&&!p.contains("signal")
               &&!p.contains("backup")&&!p.contains("/android/")){
              candidates++;candidateBytes+=bytes;
            }
          }catch(Exception ignored){skipped++;}
        }
      }
      ArrayList<Map.Entry<String,Long>> top=new ArrayList<>(totals.entrySet());
      top.sort((x,y)->Long.compare(y.getValue(),x.getValue()));
      ArrayList<Big> large=new ArrayList<>(biggest);
      large.sort((x,y)->Long.compare(y.bytes,x.bytes));
      long used=base.getTotalSpace()-base.getUsableSpace();
      long unaccounted=Math.max(0,used-visibleBytes);
      StringBuilder s=new StringBuilder();
      s.append("MAPA DO ARMAZENAMENTO COMPARTILHADO\n");
      s.append("Livre: ").append(gb(base.getUsableSpace())).append(" | ocupado no volume: ").append(gb(used)).append("\n");
      s.append("Arquivos acessíveis indexados: ").append(fileCount).append(" | pastas lidas: ").append(folders).append("\n");
      s.append("Dados compartilhados acessíveis: ").append(gb(visibleBytes)).append("\n");
      s.append("Ocupação não explicada por arquivos acessíveis: ").append(gb(unaccounted)).append("\n");
      s.append("Inclui dados privados dos apps, sistema, arquivos protegidos ou itens não lidos; NÃO significa lixo.\n\n");
      s.append("PASTAS COM MAIS DADOS\n");
      for(int i=0;i<Math.min(25,top.size());i++){
        Map.Entry<String,Long> e=top.get(i);
        s.append(i+1).append(". ").append(e.getKey()).append(": ").append(gb(e.getValue()))
         .append(" (").append(counts.getOrDefault(e.getKey(),0)).append(" arquivos)\n");
      }
      s.append("\nVÍDEOS: ").append(gb(videoBytes));
      s.append("\nINSTALADORES/TEMPORÁRIOS PARA REVISÃO: ").append(candidates).append(" itens, ").append(gb(candidateBytes));
      s.append("\nEsses arquivos NÃO são necessariamente descartáveis.\n\n");
      s.append("MAIORES ARQUIVOS (apenas visualização)\n");
      for(int i=0;i<Math.min(15,large.size());i++){
        Big b=large.get(i);
        s.append(i+1).append(". ").append(b.name).append(" — ").append(gb(b.bytes)).append(" [").append(b.category).append("]\n");
      }
      if(fileCount>=MAX_FILES)s.append("\nATENÇÃO: limite de 120 mil arquivos atingido; números incompletos.\n");
      if(stoppedThermal)s.append("\nATENÇÃO: leitura interrompida por aquecimento do aparelho.\n");
      if(skipped>0)s.append("\nPastas/itens inacessíveis ou ignorados: ").append(skipped).append("\n");
      s.append("\nNENHUM ARQUIVO FOI ALTERADO OU EXCLUÍDO.");
      ArrayList<String[]> exportRows=new ArrayList<>();
      for(Map.Entry<String,Long> e:top){
        exportRows.add(new String[]{"Pasta",e.getKey(),Long.toString(e.getValue()),
           Integer.toString(counts.getOrDefault(e.getKey(),0)),"Armazenamento compartilhado acessível"});
      }
      for(Big b:large){
        exportRows.add(new String[]{"Arquivo grande",b.name+" ["+b.category+"]",
           Long.toString(b.bytes),"1","Somente leitura"});
      }
      String visible=s.toString();
      String sharedReport="MAPA DE ARMAZENAMENTO\nLivre: "+gb(base.getUsableSpace())+
         "\nArquivos acessíveis: "+gb(visibleBytes)+"\nNão explicados por leitura: "+gb(unaccounted)+
         "\nInstaladores/temporários candidatos: "+gb(candidateBytes)+"\nPastas:\n";
      for(int i=0;i<Math.min(15,top.size());i++)
        sharedReport+=top.get(i).getKey()+": "+gb(top.get(i).getValue())+"\n";
      String finalReport=sharedReport;
      a.runOnUiThread(()->{output.setText(visible);ReportVault.setFolderAnalysis(visible,exportRows);onReport.accept(finalReport);});
    },"redmi-storage-audit").start();
  }
}
