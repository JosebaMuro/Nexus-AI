# Oficina para pymes: perfiles por gremio

Nexus-Office se puede instalar como **la oficina de una pyme**. Eliges el tipo de empresa (un *perfil*) y Nexus-Office monta su
equipo de agentes:

- **Agentes de oficina**: Recepción, Administración, Presupuestos, Compras y almacén, Prevención de riesgos,
  Personal, Calidad y Comercial (cada perfil usa los que necesita).
- **Agentes del gremio**: Herrero, Calderero, Soldador, Albañil, Fontanero, Electricista, Carpintero, Pintor,
  Cerrajero, Climatización, Mecánico, Maestro panadero y Jardinero.
- **El coordinador** (el agente principal) pasa a llamarse como la oficina («Oficina Herrería»…), conoce a qué
  se dedica la empresa y reparte el trabajo con `delegate` entre su equipo.

## Cada agente tiene su propio grafo

Cada agente, incluidos los del gremio, aprende en **su propio subgrafo aislado** (ver [Memoria](MEMORIA.md)): el
herrero no ve lo que sabe el calderero y al revés. Lo común (quién es el usuario, cómo quiere que le respondan,
los clientes y relaciones) va al grafo general, que ven todos.

Al aplicar un perfil, el grafo de cada agente **nace sembrado** con su saber hacer básico (normas, reglas de
cálculo, procedimientos: CTE DB-SUA para barandillas, desarrollo de virolas, REBT, RITE, RD 1457/1986…). A partir
de ahí crece con sus conversaciones y con los documentos que importes solo para él (*Memoria → Biblioteca →
importar para un agente*). Puedes ver cada grafo en el selector del grafo (*general*, *todos* o un agente).

## Perfiles incluidos

La oficina de bolsa e inversiones trae además una mesa de bolsa simulada para practicar sin dinero: ver [BOLSA.md](BOLSA.md).

59 perfiles: oficios y construcción, industria, comercio, hostelería, salud, servicios profesionales, bolsa e inversiones,
educación y cuidados, agro, transporte y servicios técnicos. Los agentes de oficina (recepción, administración,
presupuestos, compras, prevención, personal, calidad, comercial, agenda, protección de datos y medio ambiente) se
comparten entre perfiles; los del sector traen el conocimiento y la normativa de cada actividad.

| Perfil | Empresa | Agentes del sector | Agentes de oficina | Modelos sugeridos |
|---|---|---|---|---|
| `academia-formacion` | Academia y centro de formación | Coordinación académica | Recepción, Agenda y reservas, Administración, Comercial y marketing, Personal, Protección de datos | Qwen3-VL 8B Thinking, Qwen3-Embedding 0.6B |
| `agencia-marketing` | Agencia de marketing | Estratega de marketing, Técnico informático | Presupuestos, Administración, Recepción, Agenda y reservas, Protección de datos | SDXL-Turbo, SD-Turbo |
| `agricultura` | Explotación agrícola | Técnico agrícola, Mecánico | Medio ambiente y residuos, Administración, Compras y almacén, Comercial y marketing, Prevención de riesgos | — |
| `arquitectura-ingenieria` | Estudio de arquitectura e ingeniería | Arquitecto e ingeniero | Presupuestos, Prevención de riesgos, Calidad, Administración, Recepción, Agenda y reservas, Protección de datos | Qwen3-VL 8B Thinking, Qwen3-Embedding 0.6B |
| `asesoria-gestoria` | Asesoría y gestoría | Asesor fiscal y contable, Asesor laboral | Protección de datos, Recepción, Agenda y reservas, Administración, Comercial y marketing | Qwen3-VL 8B Thinking, Qwen3-Embedding 0.6B |
| `autoescuela` | Autoescuela | Profesor de autoescuela | Recepción, Agenda y reservas, Administración, Comercial y marketing, Protección de datos | Qwen3-VL 8B Thinking, Qwen3-Embedding 0.6B |
| `ayuda-domicilio-residencia` | Ayuda a domicilio y residencia | Coordinación sociosanitaria | Personal, Prevención de riesgos, Calidad, Compras y almacén, Administración, Recepción, Protección de datos | MedGemma 4B, Qwen3-Embedding 0.6B |
| `bar-cafeteria` | Bar y cafetería | Jefe de sala, Jefe de cocina | Calidad, Compras y almacén, Administración, Personal, Comercial y marketing | — |
| `bodega-almazara` | Bodega y almazara | Enólogo y maestro de almazara, Técnico agrícola | Calidad, Medio ambiente y residuos, Comercial y marketing, Compras y almacén, Administración, Personal | — |
| `bolsa-inversiones` | Bolsa e inversiones | Analista de mercados, Gestor de carteras, Operador de mesa, Riesgos y cumplimiento, Fiscalidad de inversiones | Recepción, Agenda y reservas, Administración, Comercial y marketing, Protección de datos | Qwen3-VL 8B Thinking, Qwen3-Embedding 0.6B |
| `caldereria` | Calderería industrial | Calderero, Soldador | Calidad, Presupuestos, Administración, Compras y almacén, Prevención de riesgos, Personal | — |
| `carniceria-charcuteria` | Carnicería y charcutería | Maestro carnicero, Encargado de tienda | Calidad, Compras y almacén, Administración, Comercial y marketing | — |
| `carpinteria` | Carpintería y ebanistería | Carpintero, Pintor | Recepción, Presupuestos, Administración, Compras y almacén, Prevención de riesgos | SDXL-Turbo, SD-Turbo |
| `carpinteria-aluminio-pvc` | Carpintería de aluminio y PVC | Técnico de carpintería de aluminio y PVC, Cristalero, Cerrajero | Recepción, Presupuestos, Administración, Compras y almacén, Prevención de riesgos | — |
| `catering-eventos` | Catering y eventos | Organizador de eventos, Jefe de cocina, Jefe de sala | Presupuestos, Agenda y reservas, Calidad, Compras y almacén, Administración, Personal, Comercial y marketing | SDXL-Turbo, SD-Turbo |
| `centro-estetica` | Centro de estética | Esteticista | Agenda y reservas, Recepción, Compras y almacén, Administración, Comercial y marketing, Calidad | — |
| `cerrajeria` | Cerrajería y carpintería metálica | Cerrajero, Herrero, Soldador | Recepción, Presupuestos, Administración, Compras y almacén | — |
| `clinica-dental` | Clínica dental | Coordinación clínica dental | Recepción, Agenda y reservas, Administración, Compras y almacén, Calidad, Personal, Protección de datos | MedGemma 4B, Qwen3-Embedding 0.6B |
| `comercio-minorista` | Comercio minorista (tienda) | Encargado de tienda | Compras y almacén, Administración, Comercial y marketing, Recepción, Personal | — |
| `construccion` | Construcción y obra nueva | Albañil, Herrero, Electricista, Fontanero | Prevención de riesgos, Calidad, Presupuestos, Administración, Compras y almacén, Personal | — |
| `control-plagas` | Control de plagas | Técnico de control de plagas, Responsable de limpieza | Medio ambiente y residuos, Recepción, Presupuestos, Administración, Calidad, Prevención de riesgos | — |
| `cristaleria` | Cristalería | Cristalero, Técnico de carpintería de aluminio y PVC | Recepción, Presupuestos, Administración, Compras y almacén, Prevención de riesgos | — |
| `despacho-abogados` | Despacho de abogados | Apoyo jurídico | Protección de datos, Recepción, Agenda y reservas, Administración, Presupuestos | Qwen3-VL 8B Thinking, Qwen3-Embedding 0.6B |
| `electricidad` | Instalaciones eléctricas | Electricista | Recepción, Presupuestos, Administración, Compras y almacén, Prevención de riesgos | — |
| `escuela-infantil` | Escuela infantil | Coordinación pedagógica infantil | Recepción, Administración, Personal, Calidad, Compras y almacén, Protección de datos, Prevención de riesgos | — |
| `estructuras-metalicas` | Estructuras metálicas y montajes | Herrero, Calderero, Soldador | Calidad, Presupuestos, Administración, Compras y almacén, Prevención de riesgos, Personal | — |
| `estudio-fotografia` | Estudio de fotografía | Fotógrafo | Agenda y reservas, Presupuestos, Recepción, Administración, Comercial y marketing | SDXL-Turbo, SD-Turbo |
| `farmacia` | Farmacia | Gestión de farmacia, Encargado de tienda | Compras y almacén, Administración, Calidad, Personal, Comercial y marketing | MedGemma 4B, Qwen3-Embedding 0.6B |
| `fisioterapia` | Centro de fisioterapia | Coordinación de fisioterapia | Agenda y reservas, Recepción, Administración, Comercial y marketing, Protección de datos | MedGemma 4B, Qwen3-Embedding 0.6B |
| `floristeria` | Floristería | Florista | Agenda y reservas, Recepción, Compras y almacén, Presupuestos, Administración, Comercial y marketing | SDXL-Turbo, SD-Turbo |
| `fontaneria-climatizacion` | Fontanería y climatización | Fontanero, Climatización, Electricista | Recepción, Presupuestos, Administración, Compras y almacén, Prevención de riesgos | — |
| `fruteria-alimentacion` | Frutería y alimentación | Frutero, Encargado de tienda | Calidad, Compras y almacén, Administración, Comercial y marketing | — |
| `ganaderia` | Explotación ganadera | Técnico ganadero, Técnico agrícola, Coordinación veterinaria | Medio ambiente y residuos, Administración, Compras y almacén, Calidad, Prevención de riesgos | — |
| `gestion-residuos` | Gestión de residuos | Jefe de tráfico, Mecánico | Medio ambiente y residuos, Recepción, Administración, Comercial y marketing, Calidad, Personal, Prevención de riesgos | — |
| `herreria` | Herrería y forja | Herrero, Soldador | Recepción, Presupuestos, Administración, Compras y almacén, Prevención de riesgos | — |
| `hotel-alojamiento` | Hotel y alojamiento turístico | Jefe de alojamiento | Agenda y reservas, Recepción, Administración, Comercial y marketing, Personal, Compras y almacén, Calidad | — |
| `imprenta-artes-graficas` | Imprenta y artes gráficas | Jefe de producción gráfica | Medio ambiente y residuos, Recepción, Presupuestos, Administración, Compras y almacén, Comercial y marketing, Calidad | SDXL-Turbo, SD-Turbo |
| `inmobiliaria` | Agencia inmobiliaria | Agente inmobiliario | Comercial y marketing, Recepción, Agenda y reservas, Administración, Protección de datos | Qwen3-VL 8B Thinking, Qwen3-Embedding 0.6B |
| `instalaciones-solares` | Instalaciones solares | Instalador solar, Electricista, Climatización | Recepción, Presupuestos, Administración, Compras y almacén, Prevención de riesgos | — |
| `jardineria` | Jardinería y paisajismo | Jardinero, Fontanero | Recepción, Presupuestos, Administración, Compras y almacén, Prevención de riesgos | — |
| `lavanderia-tintoreria` | Lavandería y tintorería | Tintorero | Recepción, Compras y almacén, Administración, Prevención de riesgos, Comercial y marketing | — |
| `limpieza-profesional` | Limpieza profesional | Responsable de limpieza, Técnico de control de plagas | Recepción, Presupuestos, Administración, Compras y almacén, Personal, Calidad, Prevención de riesgos | — |
| `mantenimiento-integral` | Mantenimiento integral y multiservicios | Albañil, Fontanero, Electricista, Pintor, Cerrajero, Climatización | Recepción, Presupuestos, Administración, Prevención de riesgos, Personal | — |
| `marmoleria-piedra` | Marmolería y piedra | Marmolista, Albañil | Medio ambiente y residuos, Recepción, Presupuestos, Administración, Compras y almacén, Prevención de riesgos | — |
| `mudanzas` | Mudanzas y guardamuebles | Jefe de mudanzas, Jefe de tráfico, Carpintero | Recepción, Presupuestos, Administración, Personal, Prevención de riesgos | — |
| `optica` | Óptica | Óptico-optometrista, Encargado de tienda | Agenda y reservas, Compras y almacén, Administración, Comercial y marketing, Calidad | MedGemma 4B, Qwen3-Embedding 0.6B |
| `panaderia` | Panadería y obrador | Maestro panadero | Calidad, Compras y almacén, Administración, Personal, Comercial y marketing, Recepción | — |
| `peluqueria-barberia` | Peluquería y barbería | Peluquero estilista | Agenda y reservas, Recepción, Compras y almacén, Administración, Comercial y marketing, Personal | — |
| `pintura` | Pintura y decoración | Pintor, Albañil | Recepción, Presupuestos, Administración, Compras y almacén, Prevención de riesgos | SDXL-Turbo, SD-Turbo |
| `piscinas` | Piscinas | Técnico de piscinas, Fontanero, Electricista, Albañil | Recepción, Presupuestos, Administración, Compras y almacén, Prevención de riesgos | — |
| `reformas` | Albañilería y reformas | Albañil, Fontanero, Electricista, Pintor | Recepción, Presupuestos, Administración, Compras y almacén, Prevención de riesgos | — |
| `restaurante` | Restaurante | Jefe de cocina, Jefe de sala | Agenda y reservas, Calidad, Compras y almacén, Administración, Personal, Comercial y marketing | — |
| `servicios-informaticos` | Servicios informáticos | Técnico informático | Protección de datos, Recepción, Agenda y reservas, Presupuestos, Compras y almacén, Administración | Qwen2.5-Coder 7B, Qwen3-Embedding 0.6B |
| `taller-mecanico` | Taller mecánico | Mecánico | Recepción, Compras y almacén, Presupuestos, Administración, Comercial y marketing, Prevención de riesgos | — |
| `tapiceria` | Tapicería | Tapicero, Carpintero, Jefe de taller de confección | Recepción, Presupuestos, Administración, Compras y almacén, Comercial y marketing | — |
| `telecomunicaciones` | Instalaciones de telecomunicaciones | Instalador de telecomunicaciones, Electricista | Recepción, Presupuestos, Administración, Compras y almacén, Prevención de riesgos | — |
| `textil-confeccion` | Textil y confección | Jefe de taller de confección, Tapicero | Medio ambiente y residuos, Calidad, Compras y almacén, Comercial y marketing, Presupuestos, Administración, Personal | SDXL-Turbo, SD-Turbo |
| `transporte-logistica` | Transporte y logística | Jefe de tráfico, Mecánico | Medio ambiente y residuos, Recepción, Administración, Comercial y marketing, Personal, Prevención de riesgos | — |
| `veterinaria` | Clínica veterinaria | Coordinación veterinaria | Agenda y reservas, Recepción, Compras y almacén, Administración, Comercial y marketing, Protección de datos | MedGemma 4B, Qwen3-Embedding 0.6B |

### Modelos de IA sugeridos para cada sector

Al instalar, Nexus-Office elige el modelo general según el hardware (`python -m nexus modelos`). Si además hay un modelo
específico que encaja con la oficina, lo **sugiere** una vez instalada: aparece primero en **Modelos > Recomendados**
con la etiqueta «◆ para tu oficina» y el motivo, y también al aplicar el perfil (`python -m nexus oficina <id>`)
y en `python -m nexus modelos`. No se descarga solo: lo decides tú, viendo si cabe en tu equipo.

- **Salud** (clínica dental, fisioterapia, farmacia, óptica, veterinaria, ayuda a domicilio): MedGemma 4B, entrenado
  con textos e imágenes médicas. Es apoyo para el profesional colegiado, nunca un diagnóstico.
- **Servicios informáticos**: Qwen2.5-Coder 7B, especializado en programación, scripts y redes.
- **Despachos y documentación** (asesoría, abogados, inmobiliaria, arquitectura, academia, autoescuela): el modelo de
  razonamiento grande (Qwen3-VL 8B) y el de documentos, para normativa y contratos largos.
- **Diseño e imagen** (marketing, imprenta, fotografía, floristería, pintura, carpintería, confección, eventos): el
  modelo de imagen de más calidad que permita el equipo.

Un perfil puede fijar los suyos con la clave `modelos: [{id: ..., motivo: ...}]` (ids de `nexus/models/recommended.py`).

## Elegir un perfil

**Desde la interfaz** (modo administrador): *Agentes → Perfil de oficina*, elige la empresa y pulsa **APLICAR**.

**Desde la terminal** (Windows: `venv\python.exe`, Linux/macOS: `venv/bin/python3`):

```
python -m nexus oficina                     # lista los perfiles y marca el activo
python -m nexus oficina herreria            # aplica el perfil
python -m nexus oficina herreria --reset-agentes   # y reescribe también los agentes que hayas personalizado
python -m nexus oficina --quitar            # vuelve a los agentes por defecto de Nexus-Office
```

**Al instalar**: en el instalador de Windows, el desplegable *Perfil de oficina* de la página *Opciones* (o
`/OFICINA=herreria` en modo silencioso); `sh Nexus-Office-1.8.0-linux-x86_64.run --oficina=herreria` (Linux y Raspberry
Pi); `./install.sh --oficina=herreria` (macOS y código fuente); `install.bat -Oficina herreria` (Windows desde el
código fuente); `bash install-termux.sh --oficina=herreria` (Android con Termux), o la variable de entorno
`NEXUS_OFICINA=herreria` con los scripts. Con el `.deb`: `nexus-office oficina herreria`. Ver
[Instalación](USUARIO.md#instalación).

**En Android**:
- **App Nexus-Office autónomo (APK)**: no tiene terminal; el perfil se elige en la propia app: *Más → Agentes*, botón
  **ADMIN** (PIN), *Perfil de oficina* → **APLICAR**. La APK lleva dentro todo `nexus/`, también los perfiles y
  agentes de la oficina, y cada agente estrena su grafo en el móvil.
- **Termux**: igual que en Linux (`venv/bin/python -m nexus oficina herreria`) o desde el navegador del móvil.
- **App Nexus-Office VPN** (cliente del PC): usa la oficina que tenga configurada el PC.

**Por API**: `GET /api/office/profiles` y `POST /api/office/profile {"id": "herreria"}` (administrador;
`{"id": ""}` quita el perfil).

Aplicar un perfil **no borra nada**: los agentes que no forman parte de la oficina se desactivan y conservan su
memoria; si un agente ya existía y lo habías personalizado se respeta (salvo con `--reset-agentes`); antes de
sobrescribir un YAML se guarda una copia en `data/agents/respaldo/`. El perfil activo queda en
`data/config.yaml` (`office.profile`).

## Sin internet

Los perfiles son YAML y el conocimiento sembrado va a la base SQLite local: todo funciona **100 % offline** en
Windows, macOS, Linux y Android (app autónoma y Termux). Los agentes de la oficina no usan herramientas que necesiten internet (búsqueda
web, descarga de páginas, rastreo); solo hace falta red para descargar los modelos de IA (o tráelos ya
descargados, ver [Plataformas](PLATAFORMAS.md)). Si quieres que un agente busque en internet, añádele
`web_search` en *Agentes → Editar*.

## Crear o ampliar perfiles

- **Agentes**: `nexus/office/agentes/<id>.yaml`. Mismo formato que los agentes de Nexus-Office (`name`, `description`,
  `system_prompt`, `tools`, `keywords`, `specialty`, `can_delegate_to`…) más la clave `grafo`: tripletas
  `"sujeto | relación | objeto"` con una cuarta parte opcional para la categoría (`especialidad`,
  `procedimiento`, `documento`, `experiencia`).
- **Perfiles**: `nexus/office/perfiles/<id>.yaml` con `name`, `sector`, `assistant_name`, `icon`,
  `description`, `agents` (ids del catálogo) y `coordinator.context` (a qué se dedica la empresa).

Los agentes de oficina son los de `OFFICE_ROLES` en `nexus/office/__init__.py`; el resto cuenta como gremio.
`tests/test_office.py` comprueba que todo perfil es coherente, que cada agente tiene su grafo y que el enrutado
lleva cada consulta al agente de su gremio.
