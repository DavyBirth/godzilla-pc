package com.redmi.rescue;

import android.app.Activity;
import android.os.Environment;
import android.os.SystemClock;
import android.os.StatFs;
import android.widget.TextView;
import android.os.PowerManager;
import android.content.Context;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.TimeUnit;

public final class RootStorageAudit {
    private RootStorageAudit() {}
    private static final long GIB=1024L*1024L*1024L;
    private static volatile boolean scanning=false;
    private static String gb(long bytes) {
        return String.format(Locale.forLanguageTag("pt-BR"),"%.2f GB",bytes/(double)GIB);
    }
    private static class Entry {
        String path,kind;long bytes;
        Entry(String p,String k,long b){path=p;kind=k;bytes=b;}
    }
    private static String kind(String p){
        if(p.startsWith("/data/user/0/")||p.startsWith("/data/user_de/0/"))return "App privado";
        if(p.startsWith("/data/media/"))return "Arquivos do usuário";
        if(p.startsWith("/data/app"))return "Código de aplicativos";
        if(p.startsWith("/data/dalvik-cache"))return "Compilação do Android";
        if(p.startsWith("/data/"))return "Dados protegidos do Android";
        return "Partição do sistema (separada)";
    }
    private static String friendly(String p) {
        // Avoid exposing private absolute paths: retain only package name for app folders.
        if(p.startsWith("/data/user/0/"))return "App (CE): "+p.substring("/data/user/0/".length());
        if(p.startsWith("/data/user_de/0/"))return "App (DE): "+p.substring("/data/user_de/0/".length());
        switch(p){
            case "/data/media/0":return "Armazenamento compartilhado";
            case "/data/app":return "Instalações de apps";
            case "/data/system":return "Configurações/dados do sistema";
            case "/data/misc":return "Serviços Android";
            case "/data/dalvik-cache":return "Cache compilado do Android";
            case "/data/local":return "Dados locais Android";
            case "/data/vendor":return "Dados do fabricante";
            case "/system":return "Partição /system";
            case "/product":return "Partição /product";
            case "/vendor":return "Partição /vendor";
            default:return "Outro componente protegido";
        }
    }
    public static void start(Activity activity,TextView output) {
        if(scanning){output.setText("A auditoria profunda já está em execução.");return;}
        scanning=true;
        output.setText("Solicitando permissão de superusuário (Magisk ou equivalente). Somente leitura. Não serão lidas conversas ou fotos.");
        new Thread(()->{
            String status="";
            ArrayList<Entry> list=new ArrayList<>();
            int lineCount=0;
            Process proc=null;
            try{
                // Fixed, read-only command. No user input or destructive operations.
                // User app data (/data/user/0) and DE data are separately measured.
                // /data/media/0 is included once; system partitions are labeled separately.
                String command=
                  "for p in /data/user/0/* /data/user_de/0/* /data/app /data/system /data/misc "+
                  "/data/dalvik-cache /data/media/0 /data/local /data/vendor /system /product /vendor; do "+
                  "[ -e \"$p\" ] && du -sk \"$p\" 2>/dev/null; done; echo AUDIT_END";
                proc=new ProcessBuilder("su","-c",command).redirectErrorStream(true).start();
                BufferedReader reader=new BufferedReader(new InputStreamReader(proc.getInputStream()));
                long deadline=SystemClock.elapsedRealtime()+90000L;
                boolean done=false;
                while(SystemClock.elapsedRealtime()<deadline && lineCount<5000) {
                    if(reader.ready()){
                        String line=reader.readLine();
                        if(line==null)break;
                        if("AUDIT_END".equals(line)){done=true;break;}
                        lineCount++;
                        int separator=line.indexOf('\t');
                        if(separator<0)separator=line.indexOf(' ');
                        if(separator<=0)continue;
                        try {
                            long kib=Long.parseLong(line.substring(0,separator).trim());
                            String path=line.substring(separator).trim();
                            if(path.isEmpty()||kib<0)continue;
                            list.add(new Entry(path,kind(path),kib*1024L));
                        } catch(NumberFormatException ignored){}
                    } else if(!proc.isAlive())break;
                    else Thread.sleep(120);
                }
                if(!done)status="Aviso: auditoria incompleta (permissão negada, limite de tempo ou pastas inacessíveis).";
            }catch(Exception e){status="Root indisponível ou recusado: "+e.getClass().getSimpleName();}
            finally{if(proc!=null&&proc.isAlive())proc.destroyForcibly();}
            list.sort((a,b)->Long.compare(b.bytes,a.bytes));
            StringBuilder sb=new StringBuilder();
            sb.append("AUDITORIA PROFUNDA — ROOT (SOMENTE LEITURA)\n");
            sb.append("Itens visíveis: ").append(list.size()).append("\n");
            sb.append("Não são feitas alterações, exclusões ou cópias de dados.\n\n");
            if(!status.isEmpty())sb.append(status).append("\n");
            if(list.isEmpty())sb.append("Sem informações de root. Não tente desbloquear o bootloader sem backup: o processo apaga os dados.\n");
            long totalApp=0,totalSystem=0,media=0;
            ArrayList<String[]> rows=new ArrayList<>();
            for(Entry e:list){
                if(e.kind.equals("App privado"))totalApp+=e.bytes;
                else if(e.kind.equals("Partição do sistema (separada)"))totalSystem+=e.bytes;
                else if(e.path.equals("/data/media/0"))media=e.bytes;
                rows.add(new String[]{"Root / "+e.kind,friendly(e.path),Long.toString(e.bytes),"1",
                   "Dado agregado por diretório; não se trata de lixo"});
            }
            sb.append("Dados privados de apps consultados: ").append(gb(totalApp)).append("\n");
            sb.append("Armazenamento compartilhado: ").append(gb(media)).append("\n");
            sb.append("Partições do sistema medidas separadamente: ").append(gb(totalSystem)).append("\n");
            sb.append("Não some partições separadas ao armazenamento interno.\n\n");
            sb.append("MAIORES COMPONENTES (até 60)\n");
            for(int i=0;i<Math.min(60,list.size());i++){
                Entry e=list.get(i);
                sb.append(i+1).append(". ").append(friendly(e.path)).append(": ")
                  .append(gb(e.bytes)).append(" [").append(e.kind).append("]\n");
            }
            sb.append("\nIMPORTANTE: uso de disco não significa arquivos descartáveis; diretórios inacessíveis e a reserva do sistema podem continuar sem explicação.");
            String text=sb.toString();
            activity.runOnUiThread(()->{
                output.setText(text);
                ReportVault.setRootAnalysis(text,rows);
                scanning=false;
            });
        },"root-audit").start();
    }
}
