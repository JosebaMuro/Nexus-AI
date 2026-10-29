CREATE (a:Persona {name: "Ana"}), (b:Empresa {name: "ACME"}), (a)-[:TRABAJA_EN {desde: 2020}]->(b);
MERGE (c:Ciudad {name:"Bilbao"})
MATCH (x {name:"ACME"}), (y {name:"Bilbao"}) CREATE (x)-[:EN]->(y);